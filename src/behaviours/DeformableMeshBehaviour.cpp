#include "behaviours/DeformableMeshBehaviour.hpp"
#include "graphics/Mesh.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/MeshNode.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "scene/SceneTree.hpp"
#include "core/Log.hpp"
#include "core/Profiler.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <functional>

namespace saida {

bool DeformableMeshBehaviour::containsNode(const Node* target,NodeId id) const {
    if(!node())return false;
    const auto contains=[&](auto&& self,const Node& current)->bool {
        if(&current==target)return current.id()==id;
        for(const auto& child:current.children())if(self(self,*child))return true;
        return false;
    };
    return contains(contains,*node());
}

bool DeformableMeshBehaviour::excluded(const std::string& name) const {
    size_t start = 0;
    while (start < excludedPrefixes.size()) {
        const size_t end = excludedPrefixes.find(',', start);
        const auto prefix = excludedPrefixes.substr(start, end == std::string::npos ? end : end - start);
        if (!prefix.empty() && name.rfind(prefix, 0) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

void DeformableMeshBehaviour::onReady() {
    auto subscribe = [&](CollisionObjectNode* body) {
        if (!body) return;
        listen(body->impacted, [this](const CollisionObjectNode::Impact& impact) {
            if (!node()) return;
            const auto inverse = glm::inverse(node()->worldTransform());
            const auto point = glm::vec3(inverse * glm::vec4(impact.point, 1.f));
            const auto direction = glm::mat3(inverse) * impact.direction;
            float energy=impact.energy;
            if(impact.other)if(const auto* other=impact.other->getBehaviour<DeformableMeshBehaviour>()) {
                // Share a pair's available energy by compliance; two deformable
                // bodies must not each absorb the complete incident energy.
                const float sum=stiffness+other->stiffness;
                if(std::isfinite(sum)&&sum>0.f)energy*=other->stiffness/sum;
            }
            if(!contactFilter_||contactFilter_(point,direction,energy))applyImpact(point, direction, energy);
        });
    };
    if (auto* body = node()->asCollisionObject()) subscribe(body);
    else for (const auto& child : node()->children()) subscribe(child->asCollisionObject());
}

bool DeformableMeshBehaviour::initialize() {
    if (cage_) return true;
    if (!node() || !tree()) return false;
    glm::vec3 minimum(std::numeric_limits<float>::max()), maximum(std::numeric_limits<float>::lowest());
    std::vector<Surface> surfaces;
    bool pending = false;
    std::function<void(Node&, const glm::mat4&)> collect = [&](Node& current, const glm::mat4& parent) {
        if (excluded(current.name())) return;
        const glm::mat4 matrix = &current == node() ? glm::mat4(1.f) : parent * current.localMatrix();
        if (auto* meshNode = dynamic_cast<MeshNode*>(&current); meshNode && meshNode->sourceMesh())
        for(size_t lod=0;lod<std::max(size_t(1),meshNode->lods().size());++lod) {
            auto* mesh = meshNode->sourceMesh(int(lod));
            if (!mesh->loaded()) { pending = true; return; }
            if (!mesh->deformationSource()) {
                if (!missingSourceReported_) {
                    Log::warn("DeformableMesh: source geometry not retained for '", current.name(), "'");
                    missingSourceReported_ = true;
                }
                return;
            }
            if(mesh->deformationSourceSkinned()) {
                Log::warn("DeformableMesh: skinned meshes are unsupported: ",current.name());
                continue;
            }
            // Source bounds are already prepared on the asset worker. Cage
            // setup must not scan/rebind every detailed vertex during a crash.
            const auto bounds=mesh->bounds();
            for(uint32_t corner=0;corner<8;++corner) {
                const glm::vec3 point=glm::mix(bounds.min,bounds.max,glm::bvec3(corner&1,corner&2,corner&4));
                const auto position = glm::vec3(matrix * glm::vec4(point, 1.f));
                minimum = glm::min(minimum, position);
                maximum = glm::max(maximum, position);
            }
            Surface surface;
            surface.node = meshNode;
            surface.nodeId = meshNode->id();
            surface.lodIndex = int(lod);
            surface.original = mesh;
            surface.source = mesh->deformationSource();
            surface.toRoot = matrix;
            surface.fromRoot = glm::inverse(matrix);
            surfaces.push_back(std::move(surface));
        }
        for (const auto& child : current.children()) collect(*child, matrix);
    };
    collect(*node(), glm::mat4(1.f));
    if (pending || surfaces.empty()) return false;
    DeformationSettings settings;
    settings.resolution = uint32_t(resolution);
    settings.stiffness = stiffness;
    settings.yieldEnergy = yieldEnergy;
    settings.radius = impactRadius;
    settings.maximumDisplacement = maximumDisplacement;
    settings.relaxationRate = relaxationRate;
    cage_ = std::make_unique<DeformationCage>(minimum, maximum, settings);
    if (!restoredOffsets_.empty()) {
        if (!cage_->restore(restoredOffsets_)) throw std::invalid_argument("DeformableMesh: invalid saved cage state");
        restoredOffsets_.clear();
    }
    for (auto& surface : surfaces) {
        surface.bindings.reserve(surface.source->vertices.size());
    }
    surfaces_ = std::move(surfaces);
    std::function<void(Node&, const glm::mat4&)> collectColliders = [&](Node& current, const glm::mat4& parent) {
        const glm::mat4 matrix = &current == node() ? glm::mat4(1.f) : parent * current.localMatrix();
        if (current.asCollisionObject()) for (const auto& child : current.children()) {
            auto* shape = dynamic_cast<CollisionShapeNode*>(child.get());
            if (!shape || shape->shapeType != CollisionShapeType::Box) continue;
            Collider collider;
            collider.shape = shape;
            collider.nodeId = shape->id();
            collider.toRoot = matrix;
            collider.fromRoot = glm::inverse(matrix);
            constexpr int kSamplesPerAxis = 3;
            for (int z=0;z<kSamplesPerAxis;++z) for (int y=0;y<kSamplesPerAxis;++y) for (int x=0;x<kSamplesPerAxis;++x) {
                const auto point = shape->offset + shape->halfExtents * glm::vec3(x-1,y-1,z-1);
                collider.points.push_back(point);
                collider.bindings.push_back(cage_->bind(glm::vec3(matrix * glm::vec4(point, 1.f))));
            }
            collider.last = collider.points;
            colliders_.push_back(std::move(collider));
        }
        for (const auto& child : current.children()) collectColliders(*child, matrix);
    };
    collectColliders(*node(), glm::mat4(1.f));
    return true;
}

bool DeformableMeshBehaviour::applyImpact(glm::vec3 point, glm::vec3 direction, float energy) {
    SAIDA_PROFILE_SCOPE("Deformation/Impact");
    if (!std::isfinite(energy) || energy <= yieldEnergy || !initialize()) return false;
    const float bounded = std::min(std::max(0.f, maximumImpactEnergy), energy);
    if (!cage_->impact(point, direction, bounded)) return false;
    absorbedEnergy_ = std::min(std::numeric_limits<float>::max() - bounded, absorbedEnergy_) + bounded;
    updateColliders();
    return true;
}

void DeformableMeshBehaviour::updateSurface(Surface& surface,size_t& vertexBudget) {
    SAIDA_PROFILE_SCOPE("Deformation/Vertices");
    if(!containsNode(surface.node,surface.nodeId))return;
    if(size_t(surface.lodIndex)>=std::max(size_t(1),surface.node->lods().size()))return;
    if(surface.node->sourceMesh(surface.lodIndex)!=surface.original)return;
    auto& resources=tree()->resources();
    if(surface.mutableMesh&&(!surface.mutableMesh->loaded()||resources.meshUpdatePending(surface.mutableMesh)))return;
    if(surface.revision==cage_->revision()&&!surface.work)return;
    const size_t count=surface.source->vertices.size();
    const size_t vertices=resources.takeDeformationVertices(std::min(count-surface.cursor,vertexBudget));
    if(surface.cursor<count&&!vertices)return;
    vertexBudget-=vertices;
    if(!surface.work) {
        surface.work=std::make_shared<PreparedMesh>();
        surface.work->geometry.vertices.reserve(count);
        surface.work->geometry.indices=surface.source->indices;
        surface.work->bounds.min=glm::vec3(std::numeric_limits<float>::max());
        surface.work->bounds.max=glm::vec3(std::numeric_limits<float>::lowest());
        surface.collision=std::make_shared<MeshCollisionData>();
        surface.collision->positions.reserve(count);
        surface.collision->indices=surface.source->indices;
        surface.work->collision=surface.collision;
        surface.snapshot=std::make_shared<DeformationCage>(*cage_);
    }
    const glm::mat3 toRoot(surface.toRoot),fromRoot(surface.fromRoot);
    const glm::mat3 rootNormal=glm::transpose(glm::inverse(toRoot));
    const size_t end=surface.cursor+vertices;
    for(;surface.cursor<end;++surface.cursor) {
        surface.work->geometry.vertices.push_back(surface.source->vertices[surface.cursor]);
        auto& vertex=surface.work->geometry.vertices.back();
        if(surface.cursor==surface.bindings.size())
            surface.bindings.push_back(cage_->bind(glm::vec3(surface.toRoot*glm::vec4(vertex.pos,1.f))));
        const auto& binding=surface.bindings[surface.cursor];
        const auto sample=surface.snapshot->sample(binding);
        if(sample.active) {
            vertex.pos+=fromRoot*sample.displacement;
            const glm::mat3 derivative=sample.derivative;
            const auto normal=glm::transpose(toRoot)*(glm::transpose(glm::inverse(derivative))*(rootNormal*vertex.normal));
            if(glm::dot(normal,normal)>0.f)vertex.normal=glm::normalize(normal);
            glm::vec3 tangent=fromRoot*(derivative*(toRoot*glm::vec3(vertex.tangent)));
            tangent-=vertex.normal*glm::dot(vertex.normal,tangent);
            if(glm::dot(tangent,tangent)>0.f)vertex.tangent=glm::vec4(glm::normalize(tangent),vertex.tangent.w);
        }
        surface.collision->positions.push_back(vertex.pos);
        surface.work->bounds.min=glm::min(surface.work->bounds.min,vertex.pos);
        surface.work->bounds.max=glm::max(surface.work->bounds.max,vertex.pos);
    }
    if(surface.cursor<count)return;
    auto prepared=std::move(surface.work);
    const uint64_t revision=surface.snapshot->revision();
    surface.collision.reset();surface.snapshot.reset();surface.cursor=0;
    if(!surface.mutableMesh) {
        surface.mutableMesh=resources.getMesh(resources.queueMemoryMesh(std::move(prepared)));
        surface.node->setRuntimeMesh(surface.mutableMesh,surface.lodIndex);
    } else if(!resources.queueMeshUpdate(surface.mutableMesh,std::move(prepared)))return;
    surface.node->markResourcesChanged(); // invalidate cached LOD bounds as geometry moves
    surface.revision=revision;
}
void DeformableMeshBehaviour::onUpdate(float dt) {
    if (!restoredOffsets_.empty() && !cage_) initialize();
    if (!cage_ || !tree() || !std::isfinite(dt) || dt <= 0.f || absorbedEnergy_ == 0.f) return;
    cage_->relax(dt);
    elapsed_ += dt;
    constexpr float kMinimumUpdateRate = 1.f;
    if (elapsed_ < 1.f / std::max(kMinimumUpdateRate, updateRate)) return;
    elapsed_ = 0.f;
    updateColliders();
    size_t vertexBudget=size_t(std::max(0,maximumVerticesPerUpdate));
    for (auto& surface : surfaces_) updateSurface(surface,vertexBudget);
}

void DeformableMeshBehaviour::updateColliders() {
    if (!updateCollision || !cage_) return;
    SAIDA_PROFILE_SCOPE("Deformation/Colliders");
    for (auto& collider : colliders_) {
        if(collider.revision==cage_->revision()||!containsNode(collider.shape,collider.nodeId))continue;
        collider.revision=cage_->revision();
        auto points = std::make_shared<std::vector<glm::vec3>>(collider.points);
        float distance = 0.f;
        for (size_t i=0;i<points->size();++i) {
            (*points)[i] += glm::mat3(collider.fromRoot) * cage_->offset(collider.bindings[i]);
            distance = std::max(distance, glm::distance((*points)[i], collider.last[i]));
        }
        if (distance < std::max(.001f, collisionUpdateDistance)) continue;
        collider.last = *points;
        collider.shape->setConvexHullPoints(std::move(points));
    }
}

void DeformableMeshBehaviour::resetDeformation() {
    if (cage_) cage_->reset();
    absorbedEnergy_ = 0.f;
    restoredOffsets_.clear();
    for (auto& surface : surfaces_) {
        if(containsNode(surface.node,surface.nodeId))surface.node->setRuntimeMesh(nullptr,surface.lodIndex);
        surface.mutableMesh = nullptr;
        surface.revision = 0;
        surface.work.reset();surface.collision.reset();surface.snapshot.reset();surface.cursor=0;
    }
    for (auto& collider : colliders_) {
        if(containsNode(collider.shape,collider.nodeId))collider.shape->setConvexHullPoints(nullptr);
        collider.last = collider.points;
        collider.revision=0;
    }
    surfaces_.clear();colliders_.clear();cage_.reset();
}
void DeformableMeshBehaviour::onDestroy() {
    resetDeformation();
}

void DeformableMeshBehaviour::save(nlohmann::json& j) const {
    reflect::saveObject(*this, j);
    j["stateVersion"] = 1;
    j["absorbedEnergy"] = absorbedEnergy_;
    j["offsets"] = nlohmann::json::array();
    const auto& offsets = cage_ ? cage_->permanentOffsets() : restoredOffsets_;
    for (auto offset : offsets) j["offsets"].push_back({offset.x, offset.y, offset.z});
}
void DeformableMeshBehaviour::load(const nlohmann::json& j) {
    resetDeformation();
    reflect::loadObject(*this, j);
    if (j.value("stateVersion", 1) != 1) throw std::invalid_argument("unsupported deformation state version");
    restoredOffsets_.clear();
    const auto offsets = j.value("offsets", nlohmann::json::array());
    constexpr size_t kMaximumCagePoints = 512;
    if (!offsets.is_array() || offsets.size() > kMaximumCagePoints) throw std::invalid_argument("invalid deformation state size");
    for (const auto& offset : offsets) {
        if (!offset.is_array() || offset.size() != 3) throw std::invalid_argument("invalid deformation offset");
        const glm::vec3 value(offset[0].get<float>(), offset[1].get<float>(), offset[2].get<float>());
        if (!std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z)) throw std::invalid_argument("nonfinite deformation state");
        restoredOffsets_.push_back(value);
    }
    absorbedEnergy_ = j.value("absorbedEnergy", 0.f);
    if (!std::isfinite(absorbedEnergy_) || absorbedEnergy_ < 0.f) throw std::invalid_argument("invalid deformation energy");
}

void DeformableMeshBehaviour::describe(reflect::TypeBuilder<DeformableMeshBehaviour>& t) {
    t.property("stiffness", &DeformableMeshBehaviour::stiffness).range(1, 10000000);
    t.property("yieldEnergy", &DeformableMeshBehaviour::yieldEnergy).range(0, 1000000);
    t.property("impactRadius", &DeformableMeshBehaviour::impactRadius).range(.01, 100);
    t.property("maximumDisplacement", &DeformableMeshBehaviour::maximumDisplacement).range(.01, 100);
    t.property("relaxationRate", &DeformableMeshBehaviour::relaxationRate).range(.1, 100);
    t.property("updateRate", &DeformableMeshBehaviour::updateRate).range(1, 60);
    t.property("maximumVerticesPerUpdate", &DeformableMeshBehaviour::maximumVerticesPerUpdate).range(0, 1048576);
    t.property("maximumImpactEnergy", &DeformableMeshBehaviour::maximumImpactEnergy).range(0, 10000000);
    t.property("resolution", &DeformableMeshBehaviour::resolution).range(2, 8);
    t.property("excludedPrefixes", &DeformableMeshBehaviour::excludedPrefixes);
    t.property("updateCollision", &DeformableMeshBehaviour::updateCollision);
    t.property("collisionUpdateDistance", &DeformableMeshBehaviour::collisionUpdateDistance).range(.001, 1);
}

} // namespace saida
