#pragma once

#include "behaviours/LODGroupBehaviour.hpp"
#include "graphics/Material.hpp"
#include "graphics/Mesh.hpp"
#include "nodes/MeshNode.hpp"
#include "physics/CollisionObjectNode.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace saida {
namespace gi_detail {
// One node's share of the walk; `recurse(node, meshes, lodRoot)` collects a
// child or a LOD level, so the plain walk and the cached one decide alike.
template <class Recurse>
bool contributorsOf(Node& root, std::vector<MeshNode*>& meshes, bool lodRoot, Recurse&& recurse) {
    if (!root.enabled() || (!lodRoot && !root.visible())) return false;
    if (auto* body = root.asCollisionObject(); body && body->motion() != BodyMotion::Static)
        return false;
    bool pending = false;
    if (auto* node = dynamic_cast<MeshNode*>(&root)) {
        auto* mesh = node->sourceMesh();
        auto* material = node->material();
        if (mesh && material && node->meshEnabled() && !mesh->skinned() &&
            !material->desc().billboardColumns) {
            meshes.push_back(node);
            pending |= !mesh->loaded();
        }
    }
    auto* group = root.getBehaviour<LODGroupBehaviour>();
    Node* selected = nullptr;
    std::vector<Node*> levels;
    if (group && group->enabled()) {
        for (size_t i = 0; i < group->levels().size(); ++i) {
            Node* level = root.findByPath(group->levels()[i].path);
            if (!level || level->parent() != &root) continue;
            levels.push_back(level);
            if (selected || !group->levelReady(int(i))) continue;
            std::vector<MeshNode*> candidate;
            const bool loading = recurse(*level, candidate, true);
            pending |= loading;
            if (!loading && !candidate.empty()) {
                selected = level;
                meshes.insert(meshes.end(), candidate.begin(), candidate.end());
            }
        }
    }
    for (const auto& child : root.children()) {
        if (std::find(levels.begin(), levels.end(), child.get()) != levels.end()) continue;
        pending |= recurse(*child, meshes, false);
    }
    return pending;
}
} // namespace gi_detail

// Select one resident geometric representation independently of screen LOD.
// Only the selected LOD root's visibility is overridden; authored visibility,
// disabled subtrees and dynamic bodies still exclude their geometry. Returns
// whether a mesh it would select is still loading.
inline bool collectGIContributors(Node& root, std::vector<MeshNode*>& meshes,
                                  bool lodRoot = false) {
    return gi_detail::contributorsOf(root, meshes, lodRoot, [](Node& n, std::vector<MeshNode*>& m, bool l) {
        return collectGIContributors(n, m, l);
    });
}

// The same selection, walked again only where the scene changed. A node's
// subtree revision moves with any change below it (Node::markChanged:
// children, visibility, enabling, LOD readiness), so a branch whose revision
// stands and that waited on no mesh answers from its last walk: an animal
// switching LOD re-walks its own branch and its ancestors' child lists, not
// the world. Leaves are not cached; branches are.
class GIContributorCache {
public:
    bool collect(Node& root, std::vector<MeshNode*>& meshes) {
        // Branches that left the scene are never looked up again (a new node
        // carries a newer revision); past twice the live set, start afresh.
        if (entries_[0].size() + entries_[1].size() > 2 * live_ + 4096) clear();
        const bool full = entries_[0].empty() && entries_[1].empty();
        walked_ = 0;
        const bool pending = walk(root, meshes, false);
        if (full) live_ = entries_[0].size() + entries_[1].size();
        return pending;
    }
    void clear() { entries_[0].clear(); entries_[1].clear(); }
    // Nodes the last collect() decided afresh, and branches it remembers. In
    // central Paris with traffic and animals: ~8 000 branches holding ~40 000
    // contributor pointers, about 1 800 nodes walked on a busy frame.
    size_t walked() const { return walked_; }
    size_t branches() const { return entries_[0].size() + entries_[1].size(); }

private:
    struct Entry { uint64_t revision = 0; bool pending = true; std::vector<MeshNode*> meshes; };
    bool walk(Node& node, std::vector<MeshNode*>& meshes, bool lodRoot) {
        if (node.children().empty()) {
            ++walked_;
            return gi_detail::contributorsOf(node, meshes, lodRoot, [this](Node& n, std::vector<MeshNode*>& m, bool l) {
                return walk(n, m, l);
            });
        }
        // References into an unordered_map survive its rehashing.
        Entry& entry = entries_[lodRoot ? 1 : 0][&node];
        if (!entry.pending && entry.revision == node.subtreeRevision()) {
            meshes.insert(meshes.end(), entry.meshes.begin(), entry.meshes.end());
            return false;
        }
        ++walked_;
        entry.meshes.clear();
        entry.pending = gi_detail::contributorsOf(node, entry.meshes, lodRoot, [this](Node& n, std::vector<MeshNode*>& m, bool l) {
            return walk(n, m, l);
        });
        entry.revision = node.subtreeRevision();
        meshes.insert(meshes.end(), entry.meshes.begin(), entry.meshes.end());
        return entry.pending;
    }
    std::unordered_map<const Node*, Entry> entries_[2];  // by whether the node is a selected LOD root
    size_t live_ = 0, walked_ = 0;
};
} // namespace saida
