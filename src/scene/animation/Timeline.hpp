#pragma once

#include "core/Reflection.hpp"

#include <algorithm>
#include <cmath>
#include <vector>
#include <string>
#include <memory>

#include <glm/gtc/quaternion.hpp>

namespace saida {

// Base class for a generic track in a timeline.
class TimelineTrack {
public:
    virtual ~TimelineTrack() = default;
    
    // Evaluate the track at a specific time.
    virtual void evaluate(float time) = 0;
};

// Reflection-driven property track for generic timeline animation.
class TimelinePropertyTrack : public TimelineTrack {
public:
    explicit TimelinePropertyTrack(std::string targetPath) : targetPath_(std::move(targetPath)) {}
    TimelinePropertyTrack(void* target, const reflect::TypeDesc& type, std::string propertyName)
        : targetPath_(type.name + "." + propertyName) {
        bind(target, type, std::move(propertyName));
    }

    template <typename T>
    TimelinePropertyTrack(T& target, std::string propertyName)
        : TimelinePropertyTrack(&target, reflect::localDesc<T>(), std::move(propertyName)) {}

    void bind(void* target, const reflect::TypeDesc& type, std::string propertyName) {
        target_ = target;
        type_ = &type;
        propertyName_ = std::move(propertyName);
        property_ = type_->findProperty(propertyName_);
    }

    template <typename T>
    void bind(T& target, std::string propertyName) {
        bind(&target, reflect::localDesc<T>(), std::move(propertyName));
    }

    void addKey(float time, nlohmann::json value) {
        keys_.push_back(Keyframe{time, std::move(value)});
        std::sort(keys_.begin(), keys_.end(),
                  [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    }

    void evaluate(float time) override {
        if (!target_ || !property_ || keys_.empty()) return;
        property_->set(target_, sample(time));
    }

    const std::string& targetPath() const { return targetPath_; }

private:
    struct Keyframe {
        float time = 0.0f;
        nlohmann::json value;
    };

    static float number(const nlohmann::json& j, float fallback = 0.0f) {
        return j.is_number() ? j.get<float>() : fallback;
    }

    static nlohmann::json lerpArray(const nlohmann::json& a, const nlohmann::json& b,
                                    float t, std::size_t count) {
        nlohmann::json out = nlohmann::json::array();
        for (std::size_t i = 0; i < count; ++i)
            out.push_back(number(a.at(i)) * (1.0f - t) + number(b.at(i)) * t);
        return out;
    }

    nlohmann::json sample(float time) const {
        if (time <= keys_.front().time) return keys_.front().value;
        if (time >= keys_.back().time) return keys_.back().value;

        auto upper = std::upper_bound(keys_.begin(), keys_.end(), time,
            [](float t, const Keyframe& k) { return t < k.time; });
        const Keyframe& b = *upper;
        const Keyframe& a = *(upper - 1);
        const float span = std::max(1e-6f, b.time - a.time);
        const float t = std::clamp((time - a.time) / span, 0.0f, 1.0f);

        const std::string& kind = property_->kind;
        if (kind == "float")
            return number(a.value) * (1.0f - t) + number(b.value) * t;
        if (kind == "int") {
            const float v = number(a.value) * (1.0f - t) + number(b.value) * t;
            return static_cast<int>(std::lround(v));
        }
        if ((kind == "vec3" || kind == "vec4") &&
            a.value.is_array() && b.value.is_array()) {
            const std::size_t count = kind == "vec3" ? 3u : 4u;
            if (a.value.size() >= count && b.value.size() >= count)
                return lerpArray(a.value, b.value, t, count);
        }
        if (kind == "quat" && a.value.is_array() && b.value.is_array() &&
            a.value.size() >= 4 && b.value.size() >= 4) {
            glm::quat qa(number(a.value[3]), number(a.value[0]),
                         number(a.value[1]), number(a.value[2]));
            glm::quat qb(number(b.value[3]), number(b.value[0]),
                         number(b.value[1]), number(b.value[2]));
            glm::quat q = glm::normalize(glm::slerp(qa, qb, t));
            return nlohmann::json::array({q.x, q.y, q.z, q.w});
        }

        // Non-interpolable values (bool, string, enum, asset, json) step at the
        // next key. This matches how animation event/state tracks usually behave.
        return t < 1.0f ? a.value : b.value;
    }

    std::string targetPath_;
    std::string propertyName_;
    void* target_ = nullptr;
    const reflect::TypeDesc* type_ = nullptr;
    const reflect::PropertyDesc* property_ = nullptr;
    std::vector<Keyframe> keys_;
};

// Cinematic/generic multi-track animation, separate from skeletal AnimationClip.
class Timeline {
public:
    void addTrack(std::unique_ptr<TimelineTrack> track) {
        tracks_.push_back(std::move(track));
    }

    void evaluate(float time) {
        for (auto& track : tracks_) {
            track->evaluate(time);
        }
    }

    float duration() const { return duration_; }
    void setDuration(float d) { duration_ = d; }

private:
    std::vector<std::unique_ptr<TimelineTrack>> tracks_;
    float duration_ = 0.0f;
};

} // namespace saida
