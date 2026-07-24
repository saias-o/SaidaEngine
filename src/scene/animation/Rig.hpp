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

private:
    std::vector<Bone> bones_;
    std::vector<uint32_t> evaluationOrder_;
    bool finalized_ = false;
};

} // namespace saida
