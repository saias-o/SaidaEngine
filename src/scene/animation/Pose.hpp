#pragma once

#include <vector>
#include <cstddef>
#include <glm/glm.hpp>
#include "scene/Transform.hpp"
#include "scene/animation/Rig.hpp"

namespace saida {

struct LocalPose {
    std::vector<Transform> localTransforms;

    void resize(size_t boneCount) {
        localTransforms.resize(boneCount);
    }
};

// Represents a skeleton's pose in global (world or object) space.
struct GlobalPose {
    std::vector<glm::mat4> globalMatrices;
    std::vector<glm::mat4> skinningMatrices; // globalMatrix * inverseBindMatrix

    void resize(size_t boneCount) {
        globalMatrices.resize(boneCount);
        skinningMatrices.resize(boneCount);
    }

    // Computes the global pose from a local pose.
    // baseTransform is the transform of the root entity (typically object space).
    void computeFrom(const LocalPose& localPose, const Rig& rig, const glm::mat4& baseTransform = glm::mat4(1.0f));
};

} // namespace saida
