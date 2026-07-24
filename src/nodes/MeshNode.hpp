#pragma once

#include "scene/Node.hpp"
#include "scene/MeshLod.hpp"

#include <string>
#include <utility>
#include <vector>

namespace saida {

class Mesh;
class Material;

// A node that draws a mesh with a material (cf. Godot MeshInstance3D / Unity
// MeshRenderer). Both are shared resources owned by the ResourceManager; the
// node only references them.
class MeshNode : public Node {
public:
    MeshNode() : Node("MeshNode"), mesh_(nullptr), material_(nullptr) {}
    MeshNode(std::string name, Mesh* mesh, Material* material)
        : Node(std::move(name)), mesh_(mesh), material_(material) {}

    const char* typeName() const override { return "MeshNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

    Mesh* mesh() const override { return mesh_; }
    Material* material() const override { return material_; }

    void setMesh(Mesh* mesh) { mesh_ = mesh; }
    void setMaterial(Material* material) { material_ = material; }

    // LOD chain (LOD0 = base mesh/material). Empty → single-LOD behaviour.
    const std::vector<MeshLodLevel>& lods() const { return lods_; }
    std::vector<MeshLodLevel>& lods() { return lods_; }
    void setLods(std::vector<MeshLodLevel> levels);
    bool hasLods() const { return lods_.size() > 1; }

    // Runtime: last LOD picked by the renderer (for inspector debug).
    int activeLodIndex() const { return activeLodIndex_; }
    void setActiveLodIndex(int idx) const { activeLodIndex_ = idx; }

    Mesh* meshForLod(int lodIndex) const;
    Material* materialForLod(int lodIndex) const;
    int selectLodIndex(float screenCoverage) const;

    bool& castShadows() { return castShadows_; }
    bool castShadows() const { return castShadows_; }

    bool outlineEnabled() const { return outlineEnabled_; }
    void setOutlineEnabled(bool enabled) { outlineEnabled_ = enabled; }
    glm::vec4& outlineColor() { return outlineColor_; }
    const glm::vec4& outlineColor() const { return outlineColor_; }
    float& outlineWidth() { return outlineWidth_; }
    float outlineWidth() const { return outlineWidth_; }

    bool meshEnabled() const { return meshEnabled_; }
    void setMeshEnabled(bool enabled) {
        if (meshEnabled_ != enabled) {
            meshEnabled_ = enabled;
            g_hierarchyVersion++;
        }
    }

    // Durable resource identity (Track 1-F): the serialized subset
    // {mesh, texture, baseColor, metallic, roughness, ao, emissive, shader,
    // lods} captured as-is by headless deserialization (which cannot resolve
    // resources) and re-emitted by headless serialization — the collaboration
    // fold no longer loses rendering data. Runtimes with a ResourceManager
    // (web) resolve it into an actual mesh/material.
    const std::string& durableResourceRefs() const { return durableResourceRefs_; }
    void setDurableResourceRefs(std::string refsJson) {
        durableResourceRefs_ = std::move(refsJson);
    }
    // Captures the durable subset from a serialized node document
    // (called by both deserialization paths, full and headless).
    void captureDurableResourceRefs(const nlohmann::json& j);

private:
    Mesh* mesh_;
    Material* material_;
    std::vector<MeshLodLevel> lods_;
    mutable int activeLodIndex_ = 0;
    bool castShadows_ = true;
    bool outlineEnabled_ = false;
    glm::vec4 outlineColor_{0.02f, 0.02f, 0.02f, 1.0f};
    float outlineWidth_ = 3.0f;  // screen pixels
    bool meshEnabled_ = true;
    std::string durableResourceRefs_;
};

} // namespace saida
