#pragma once

#include <vector>
#include <string>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <cstddef>
#include "scene/Transform.hpp"
#include "scene/animation/Pose.hpp"

namespace saida {

// Targets for skeletal animation tracks
enum class TrackTarget {
    Translation,
    Rotation,
    Scale
};

enum class TrackInterpolation {
    Step,
    Linear,
    CubicSpline
};

// Base interface for a bone animation track
class AnimTrack {
public:
    virtual ~AnimTrack() = default;
    
    // outTransform must already contain the default pose for untouched channels.
    virtual void evaluate(float time, Transform& outTransform) const = 0;
};

// Templated track to handle vec3 (Translation/Scale) or quat (Rotation)
template <typename T>
class TypedAnimTrack : public AnimTrack {
public:
    TrackTarget target;
    TrackInterpolation interpolation = TrackInterpolation::Linear;
    std::vector<float> timestamps;
    std::vector<T> values;

    std::vector<T> inTangents;
    std::vector<T> outTangents;

    void evaluate(float time, Transform& outTransform) const override;
};

// AnimationClip represents an immutable, read-only buffer of animation data.
// It is completely decoupled from any specific entity, allowing it to be shared (e.g. Mixamo run.glb).
class AnimationClip {
public:
    AnimationClip(std::string name, float duration) 
        : name_(std::move(name)), duration_(duration) {}

    // Adds a track for a given bone name.
    // In a retargeted workflow, this name maps to a standard Rig bone name.
    void addTrack(const std::string& boneName, std::unique_ptr<AnimTrack> track);

    float duration() const { return duration_; }
    const std::string& name() const { return name_; }

    const std::vector<std::unique_ptr<AnimTrack>>* getTracks(const std::string& boneName) const;

    // Names of all bones/joints this clip animates — used to auto-build a
    // RetargetMap between a clip and a differently-named rig.
    std::vector<std::string> boneNames() const;

private:
    std::string name_;
    float duration_ = 0.0f;
    // Storage for bone tracks by name.
    std::unordered_map<std::string, std::vector<std::unique_ptr<AnimTrack>>> boneTracks_;
};

} // namespace saida
