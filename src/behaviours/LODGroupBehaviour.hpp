#pragma once

#include "core/ReflectionFwd.hpp"
#include "scene/Behaviour.hpp"
#include "scene/MeshLod.hpp"
#include <algorithm>
#include <string>

namespace saida {

// Selects one child representation at render time. Empty groups remain the
// editor marker for MeshNode's existing per-mesh LOD chains.
class LODGroupBehaviour : public Behaviour {
public:
    const char* typeName() const override { return "LOD Group"; }

    struct Level { std::string path; float minCoverage = 0.f; };
    void setLevels(std::vector<Level> levels);
    const std::vector<Level>& levels() const { return levels_; }
    int activeLevel() const { return active_; }
    // Coverage plus the finest-level budget, independent of upload readiness.
    // Updated by updateForView; a streamer uses it to request its next level.
    int desiredLevel() const { return desired_; }
    // Runtime-only coverage bounds in the group's local frame. With explicit
    // bounds, the Near child may stay empty while its geometry is streamed.
    void setBounds(const Aabb& local);
    // Runtime-only availability, true by default. A level with an unloaded mesh
    // is unavailable even when its caller has marked it ready.
    void setLevelReady(int level, bool ready);
    // The finest level this group may show (0, the default, lets coverage
    // decide alone). Coarser levels still follow coverage. A caller with a
    // budget for its most detailed level -- the few nearest trees of a
    // forest -- holds the rest of its groups back with it. Runtime only.
    void setFinestLevel(int level) { finest_ = level < 0 ? 0 : level; }
    int finestLevel() const { return finest_; }
    // The level shown when coverage selects `selected` among `count` levels.
    static int shownLevel(int selected, int finest, int count);

    // Cross-fade: seconds over which a change of level dissolves one level
    // into the other instead of swapping them (0, the default, swaps). Both
    // levels are drawn during the fade, each through a complementary screen
    // door (MeshNode::lodFade, shader.frag): no blending, no sorting, one
    // shadow, and nothing extra once the fade is over. Serialized.
    void setCrossFade(float seconds) { crossFade_ = seconds > 0.f ? seconds : 0.f; }
    float crossFade() const { return crossFade_; }
    // The level fading out, or -1 when no fade runs.
    int fadingLevel() const { return fading_; }
    // The share of pixels the incoming level draws, t, as MeshNode::lodFade
    // stores it for the incoming (t) and the outgoing (-t) level.
    static float incomingFade(float t) { return t >= 1.f ? 1.f : std::max(t, 1e-3f); }
    static float outgoingFade(float t) { return -std::min(std::max(t, 0.f), 1.f); }
    // `frame` counts the scene's LOD updates. A group only cross-fades when
    // it was also updated the frame before: one hidden or disabled meanwhile
    // comes back at its level at once, and 0 (a camera cut) fades nothing.
    void updateForView(const glm::mat4& view, const glm::mat4& projection, uint64_t frame = 0);
    void onDisable() override;
    void onDestroy() override { onDisable(); }
    void save(nlohmann::json& out) const override;
    void load(const nlohmann::json& in) override;

    static constexpr const char* reflectName() { return "LOD Group"; }
    static void describe(reflect::TypeBuilder<LODGroupBehaviour>& t);

private:
    void resolve();
    bool levelLoaded(int level) const;
    bool levelAvailable(int level) const;
    int availableLevel() const;
    std::vector<Level> levels_;
    std::vector<MeshLodLevel> thresholds_;
    std::vector<Node*> roots_;
    std::vector<bool> ready_, loaded_;
    Aabb bounds_;
    bool explicitBounds_ = false, pendingMeshes_ = false;
    uint64_t resolvedRevision_ = 0, resolvedTransformRevision_ = 0;
    static void applyFade(Node* root, float fade);
    int active_ = -1, selected_ = -1, desired_ = -1, finest_ = 0;
    float crossFade_ = 0.f, progress_ = 1.f;
    int fading_ = -1;
    uint64_t lastFrame_ = 0;
};

} // namespace saida
