#pragma once

#include "core/ReflectionFwd.hpp"
#include "scene/Behaviour.hpp"
#include "scene/MeshLod.hpp"
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
    void updateForView(const glm::mat4& view, const glm::mat4& projection);
    void onDisable() override;
    void onDestroy() override { onDisable(); }
    void save(nlohmann::json& out) const override;
    void load(const nlohmann::json& in) override;

    static constexpr const char* reflectName() { return "LOD Group"; }
    static void describe(reflect::TypeBuilder<LODGroupBehaviour>& t);

private:
    void resolve();
    std::vector<Level> levels_;
    std::vector<MeshLodLevel> thresholds_;
    std::vector<Node*> roots_;
    Aabb bounds_;
    uint64_t resolvedRevision_ = 0, resolvedTransformRevision_ = 0;
    int active_ = -1;
};

} // namespace saida
