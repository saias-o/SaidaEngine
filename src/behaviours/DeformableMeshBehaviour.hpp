#pragma once

#include "core/Reflection.hpp"
#include "scene/Behaviour.hpp"
#include "scene/NodeId.hpp"
#include "physics/DeformationCage.hpp"
#include <memory>
#include <string>
#include <functional>
#include <utility>

namespace saida {
class MeshNode;
class Mesh;
class CollisionShapeNode;
struct MeshData;
struct PreparedMesh;
struct MeshCollisionData;

class DeformableMeshBehaviour : public Behaviour {
public:
    float stiffness = 80000.f;
    float yieldEnergy = 150.f;
    float impactRadius = .8f;
    float maximumDisplacement = .45f;
    float relaxationRate = 12.f;
    float updateRate = 15.f;
    int maximumVerticesPerUpdate = 2*1024;
    float maximumImpactEnergy = 80000.f;
    int resolution = 5;
    std::string excludedPrefixes;
    bool updateCollision = true;
    float collisionUpdateDistance = .02f;

    static constexpr const char* reflectName() { return "DeformableMesh"; }
    const char* typeName() const override { return reflectName(); }
    static void describe(reflect::TypeBuilder<DeformableMeshBehaviour>& t);
    void save(nlohmann::json& j) const override;
    void load(const nlohmann::json& j) override;
    void onReady() override;
    void onUpdate(float dt) override;
    void onDestroy() override;
    bool applyImpact(glm::vec3 localPoint, glm::vec3 localDirection, float joules);
    using ContactFilter=std::function<bool(glm::vec3,glm::vec3,float)>;
    // Runtime host policy for automatic contacts; explicit impacts remain explicit.
    void setContactFilter(ContactFilter filter) { contactFilter_=std::move(filter); }
    void resetDeformation();
    float absorbedEnergy() const { return absorbedEnergy_; }
    glm::vec3 displacementAt(glm::vec3 point) const { return cage_?cage_->offset(cage_->bind(point)):glm::vec3(0); }
private:
    struct Surface {
        MeshNode* node = nullptr;
        NodeId nodeId = kNodeInvalid;
        int lodIndex = 0;
        Mesh* original = nullptr;
        Mesh* mutableMesh = nullptr;
        glm::mat4 toRoot{1.f}, fromRoot{1.f};
        std::shared_ptr<const MeshData> source;
        std::vector<CageBinding> bindings;
        uint64_t revision = 0;
        std::shared_ptr<PreparedMesh> work;
        std::shared_ptr<MeshCollisionData> collision;
        std::shared_ptr<DeformationCage> snapshot;
        size_t cursor = 0;
    };
    struct Collider {
        CollisionShapeNode* shape = nullptr;
        NodeId nodeId = kNodeInvalid;
        glm::mat4 toRoot{1.f}, fromRoot{1.f};
        std::vector<glm::vec3> points, last;
        std::vector<CageBinding> bindings;
        uint64_t revision = 0;
    };
    bool initialize();
    bool excluded(const std::string& name) const;
    bool containsNode(const Node* target,NodeId id) const;
    void updateSurface(Surface& surface,size_t& vertexBudget);
    void updateColliders();
    std::unique_ptr<DeformationCage> cage_;
    std::vector<Surface> surfaces_;
    std::vector<Collider> colliders_;
    std::vector<glm::vec3> restoredOffsets_;
    float elapsed_ = 0.f, absorbedEnergy_ = 0.f;
    bool missingSourceReported_ = false;
    ContactFilter contactFilter_;
};

} // namespace saida
