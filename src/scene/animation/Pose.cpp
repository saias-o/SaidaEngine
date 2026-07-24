#include "scene/animation/Pose.hpp"

namespace saida {

void GlobalPose::computeFrom(const LocalPose& localPose, const Rig& rig, const glm::mat4& baseTransform) {
    const size_t boneCount = rig.bones().size();
    resize(boneCount);

    const auto& order = rig.evaluationOrder();
    if (order.size() != boneCount) return;

    for (uint32_t boneIndex : order) {
        const size_t i = boneIndex;
        const auto& bone = rig.bones()[i];

        const glm::mat4 localMat = i < localPose.localTransforms.size()
            ? localPose.localTransforms[i].matrix()
            : bone.restLocal.matrix();

        if (bone.parentIndex >= 0) {
            globalMatrices[i] = globalMatrices[bone.parentIndex] * localMat;
        } else {
            globalMatrices[i] = baseTransform * localMat;
        }

        skinningMatrices[i] = globalMatrices[i] * bone.inverseBindMatrix;
    }
}

} // namespace saida
