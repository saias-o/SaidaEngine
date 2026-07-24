#include "scene/animation/AnimationClip.hpp"
#include <algorithm>
#include <iterator>
#include <glm/gtc/quaternion.hpp> // Required for slerp

namespace saida {

template <typename T>
static T interpolate(const T& a, const T& b, float t);

template <>
glm::vec3 interpolate<glm::vec3>(const glm::vec3& a, const glm::vec3& b, float t) {
    return glm::mix(a, b, t);
}

template <>
glm::quat interpolate<glm::quat>(const glm::quat& a, const glm::quat& b, float t) {
    return glm::slerp(a, b, t);
}

// glTF cubic-spline (Hermite) between two keys. v0/v1 are the key values, m0 the
// out-tangent of the previous key, m1 the in-tangent of the next key, dt = t1-t0,
// s the normalized [0,1] factor. GLM defines +, scalar* for vec3 and quat, so the
// same expression serves both; quaternions are renormalized by the caller.
template <typename T>
static T hermite(const T& v0, const T& m0, const T& v1, const T& m1, float dt, float s) {
    const float s2 = s * s, s3 = s2 * s;
    const float h00 = 2.0f * s3 - 3.0f * s2 + 1.0f;
    const float h10 = s3 - 2.0f * s2 + s;
    const float h01 = -2.0f * s3 + 3.0f * s2;
    const float h11 = s3 - s2;
    return h00 * v0 + (h10 * dt) * m0 + h01 * v1 + (h11 * dt) * m1;
}

template <typename T>
void TypedAnimTrack<T>::evaluate(float time, Transform& outTransform) const {
    if (timestamps.empty()) return;

    if (time <= timestamps.front()) {
        if constexpr (std::is_same_v<T, glm::vec3>) {
            if (target == TrackTarget::Translation) outTransform.position = values.front();
            else if (target == TrackTarget::Scale) outTransform.scale = values.front();
        } else if constexpr (std::is_same_v<T, glm::quat>) {
            if (target == TrackTarget::Rotation) outTransform.rotation = values.front();
        }
        return;
    }
    
    if (time >= timestamps.back()) {
        if constexpr (std::is_same_v<T, glm::vec3>) {
            if (target == TrackTarget::Translation) outTransform.position = values.back();
            else if (target == TrackTarget::Scale) outTransform.scale = values.back();
        } else if constexpr (std::is_same_v<T, glm::quat>) {
            if (target == TrackTarget::Rotation) outTransform.rotation = values.back();
        }
        return;
    }

    auto it = std::upper_bound(timestamps.begin(), timestamps.end(), time);
    size_t nextIdx = std::distance(timestamps.begin(), it);
    size_t prevIdx = nextIdx - 1;

    float t0 = timestamps[prevIdx];
    float t1 = timestamps[nextIdx];
    float factor = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;

    T val;
    if (interpolation == TrackInterpolation::Step) {
        val = values[prevIdx];
    } else if (interpolation == TrackInterpolation::CubicSpline &&
               prevIdx < outTangents.size() && nextIdx < inTangents.size()) {
        val = hermite(values[prevIdx], outTangents[prevIdx],
                      values[nextIdx], inTangents[nextIdx], t1 - t0, factor);
        if constexpr (std::is_same_v<T, glm::quat>) val = glm::normalize(val);
    } else {
        val = interpolate(values[prevIdx], values[nextIdx], factor);
    }

    if constexpr (std::is_same_v<T, glm::vec3>) {
        if (target == TrackTarget::Translation) outTransform.position = val;
        else if (target == TrackTarget::Scale) outTransform.scale = val;
    } else if constexpr (std::is_same_v<T, glm::quat>) {
        if (target == TrackTarget::Rotation) outTransform.rotation = val;
    }
}

template class TypedAnimTrack<glm::vec3>;
template class TypedAnimTrack<glm::quat>;

void AnimationClip::addTrack(const std::string& boneName, std::unique_ptr<AnimTrack> track) {
    boneTracks_[boneName].push_back(std::move(track));
}

const std::vector<std::unique_ptr<AnimTrack>>* AnimationClip::getTracks(const std::string& boneName) const {
    auto it = boneTracks_.find(boneName);
    if (it != boneTracks_.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<std::string> AnimationClip::boneNames() const {
    std::vector<std::string> names;
    names.reserve(boneTracks_.size());
    for (const auto& [name, tracks] : boneTracks_) names.push_back(name);
    return names;
}

} // namespace saida
