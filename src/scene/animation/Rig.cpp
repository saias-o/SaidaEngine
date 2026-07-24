#include "scene/animation/Rig.hpp"

#include <deque>
#include <utility>

namespace saida {

void Rig::addBone(std::string name, int32_t parentIndex, const glm::mat4& invBind,
                  const Transform& restLocal) {
    bones_.push_back({std::move(name), parentIndex, invBind, restLocal});
    evaluationOrder_.clear();
    finalized_ = false;
}

bool Rig::finalize(std::string* error) {
    evaluationOrder_.clear();
    finalized_ = false;

    const size_t count = bones_.size();
    std::vector<std::vector<uint32_t>> children(count);
    std::vector<uint32_t> indegree(count, 0);
    std::deque<uint32_t> ready;

    for (uint32_t i = 0; i < count; ++i) {
        const int32_t parent = bones_[i].parentIndex;
        if (parent < -1 || parent >= static_cast<int32_t>(count) ||
            parent == static_cast<int32_t>(i)) {
            if (error) *error = "invalid parent index for bone " + bones_[i].name;
            return false;
        }
        if (parent < 0) {
            ready.push_back(i);
        } else {
            children[static_cast<size_t>(parent)].push_back(i);
            indegree[i] = 1;
        }
    }

    evaluationOrder_.reserve(count);
    while (!ready.empty()) {
        const uint32_t bone = ready.front();
        ready.pop_front();
        evaluationOrder_.push_back(bone);
        for (uint32_t child : children[bone]) {
            if (--indegree[child] == 0) ready.push_back(child);
        }
    }

    if (evaluationOrder_.size() != count) {
        evaluationOrder_.clear();
        if (error) *error = "bone hierarchy contains a cycle";
        return false;
    }

    finalized_ = true;
    return true;
}

int32_t Rig::findBoneIndex(const std::string& name) const {
    for (size_t i = 0; i < bones_.size(); ++i) {
        if (bones_[i].name == name) return static_cast<int32_t>(i);
    }
    return -1;
}

} // namespace saida
