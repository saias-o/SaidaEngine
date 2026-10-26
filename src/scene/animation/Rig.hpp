#pragma once

#include "scene/Transform.hpp"

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <glm/glm.hpp>

namespace saida {

struct Bone {
    std::string name;
    int32_t parentIndex = -1;
    glm::mat4 inverseBindMatrix{1.0f};
    Transform restLocal;
};

class Rig {
public:
    void addBone(std::string name, int32_t parentIndex, const glm::mat4& invBind,
                 const Transform& restLocal = {});
    bool finalize(std::string* error = nullptr);

    const std::vector<Bone>& bones() const { return bones_; }
    size_t boneCount() const { return bones_.size(); }
    const std::vector<uint32_t>& evaluationOrder() const { return evaluationOrder_; }
    bool finalized() const { return finalized_; }

    int32_t findBoneIndex(const std::string& name) const;

    // Where the root bones' parent stands in the file the rig came from: the
    // nodes a glTF puts above its skeleton (an armature scaled to size, a
    // container offset). Bone matrices are relative to that frame, so a mesh
    // the rig deforms is drawn in it (GLTFLoader places skinned primitives so).
    void setRootParent(const glm::mat4& frame) { rootParent_ = frame; }
    const glm::mat4& rootParent() const { return rootParent_; }

private:
    std::vector<Bone> bones_;
    glm::mat4 rootParent_{1.0f};
    std::vector<uint32_t> evaluationOrder_;
    bool finalized_ = false;
};

} // namespace saida
