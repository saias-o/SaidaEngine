#include "scene/animation/AnimationCooker.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace saida {

namespace {

constexpr uint32_t kCookerVersion = 1;
constexpr uint64_t kFnvOffset = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void hashBytes(uint64_t& h, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= kFnvPrime;
    }
}

void hashString(uint64_t& h, const std::string& s) {
    hashBytes(h, s.data(), s.size());
    hashBytes(h, "\0", 1);
}

template <typename T>
void hashValue(uint64_t& h, const T& value) {
    hashBytes(h, &value, sizeof(T));
}

// A track's authoring keys, in float, before quantization.
template <typename T>
struct SourceKeys {
    std::vector<float> times;
    std::vector<T> values;
};

float channelError(const glm::vec3& a, const glm::vec3& b) {
    const glm::vec3 d = glm::abs(a - b);
    return std::max(d.x, std::max(d.y, d.z));
}

// Angle between two rotations (tolerance metric for quaternion tracks).
float channelError(const glm::quat& a, const glm::quat& b) {
    const float d = std::min(1.0f, std::abs(glm::dot(a, b)));
    return 2.0f * std::acos(d);
}

glm::vec3 interpolateKey(const glm::vec3& a, const glm::vec3& b, float t) {
    return glm::mix(a, b, t);
}

glm::quat interpolateKey(const glm::quat& a, const glm::quat& b, float t) {
    return glm::slerp(a, b, t);
}

// Resamples a track through its source evaluation (including exact cubic
// spline behavior), at `rate` Hz over [start, end] of the keys.
template <typename T>
SourceKeys<T> resample(const TypedAnimTrack<T>& track, float rate) {
    SourceKeys<T> out;
    const float start = track.timestamps.front();
    const float end = track.timestamps.back();
    const int count = std::max(2, int(std::ceil((end - start) * rate)) + 1);
    out.times.resize(size_t(count));
    out.values.resize(size_t(count));
    Transform scratch;
    for (int i = 0; i < count; ++i) {
        const float t = start + (end - start) * float(i) / float(count - 1);
        track.evaluate(t, scratch);
        out.times[size_t(i)] = t;
        if constexpr (std::is_same_v<T, glm::vec3>) {
            out.values[size_t(i)] =
                track.target == TrackTarget::Translation ? scratch.position : scratch.scale;
        } else {
            out.values[size_t(i)] = scratch.rotation;
        }
    }
    return out;
}

template <typename T>
SourceKeys<T> extractKeys(const TypedAnimTrack<T>& track, const CookSettings& settings) {
    if (track.interpolation == TrackInterpolation::CubicSpline)
        return resample(track, settings.splineResampleRate);
    SourceKeys<T> out;
    out.times = track.timestamps;
    out.values = track.values;
    if constexpr (std::is_same_v<T, glm::quat>) {
        for (glm::quat& q : out.values) q = glm::normalize(q);
    }
    return out;
}

// Greedy reduction: keeps the farthest key such that all skipped keys are
// still reproduced within tolerance. Step tracks can only drop keys equal to
// the previous one.
template <typename T>
void reduceKeys(SourceKeys<T>& keys, TrackInterpolation interpolation, float tolerance) {
    const size_t count = keys.times.size();
    if (count <= 2) return;

    std::vector<size_t> kept;
    kept.push_back(0);
    if (interpolation == TrackInterpolation::Step) {
        for (size_t i = 1; i < count; ++i) {
            if (channelError(keys.values[i], keys.values[kept.back()]) > tolerance)
                kept.push_back(i);
        }
    } else {
        size_t anchor = 0;
        while (anchor + 1 < count) {
            size_t best = anchor + 1;
            for (size_t candidate = anchor + 2; candidate < count; ++candidate) {
                bool allWithin = true;
                for (size_t mid = anchor + 1; mid < candidate && allWithin; ++mid) {
                    const float span = keys.times[candidate] - keys.times[anchor];
                    const float factor =
                        span > 0.0f ? (keys.times[mid] - keys.times[anchor]) / span : 0.0f;
                    const T predicted =
                        interpolateKey(keys.values[anchor], keys.values[candidate], factor);
                    allWithin = channelError(predicted, keys.values[mid]) <= tolerance;
                }
                if (!allWithin) break;
                best = candidate;
            }
            kept.push_back(best);
            anchor = best;
        }
    }

    if (kept.size() == count) return;
    SourceKeys<T> reduced;
    reduced.times.reserve(kept.size());
    reduced.values.reserve(kept.size());
    for (size_t index : kept) {
        reduced.times.push_back(keys.times[index]);
        reduced.values.push_back(keys.values[index]);
    }
    keys = std::move(reduced);
}

template <typename T>
bool isConstant(const SourceKeys<T>& keys, float tolerance) {
    for (size_t i = 1; i < keys.values.size(); ++i)
        if (channelError(keys.values[i], keys.values.front()) > tolerance) return false;
    return true;
}

// Rest value of the target channel, used to drop tracks that add nothing.
template <typename T>
T restValue(const Transform& rest, TrackTarget target) {
    if constexpr (std::is_same_v<T, glm::vec3>) {
        return target == TrackTarget::Translation ? rest.position : rest.scale;
    } else {
        (void)target;
        return rest.rotation;
    }
}

uint16_t quantizeComponent(float value, float min, float scale) {
    if (scale <= 0.0f) return 0;
    return uint16_t(std::clamp(std::lround((value - min) / scale), 0l, 65535l));
}

// Fills the compact streams of a CookedClip. The vectors are passed in by
// the cooker (the only friend of CookedClip) to keep the format read-only.
class CookedClipBuilder {
public:
    CookedClipBuilder(std::vector<CookedTrack>& tracks, std::vector<CookedPage>& pages,
                      std::vector<uint16_t>& times, std::vector<uint16_t>& values,
                      const CookSettings& settings)
        : tracks_(tracks), pages_(pages), times_(times), values_(values),
          settings_(settings) {}

    template <typename T>
    void addTrack(uint16_t boneIndex, TrackTarget target, TrackInterpolation interpolation,
                  const SourceKeys<T>& keys) {
        CookedTrack track;
        track.boneIndex = boneIndex;
        track.target = target;
        track.interpolation = interpolation == TrackInterpolation::Step
                                  ? TrackInterpolation::Step
                                  : TrackInterpolation::Linear;
        track.firstPage = uint32_t(pages_.size());

        if constexpr (std::is_same_v<T, glm::vec3>) {
            glm::vec3 minValue = keys.values.front();
            glm::vec3 maxValue = keys.values.front();
            for (const glm::vec3& v : keys.values) {
                minValue = glm::min(minValue, v);
                maxValue = glm::max(maxValue, v);
            }
            track.valueMin = minValue;
            track.valueScale = (maxValue - minValue) / 65535.0f;
        }

        size_t first = 0;
        while (first < keys.times.size()) {
            const float pageStart = keys.times[first];
            size_t last = first;
            while (last + 1 < keys.times.size() &&
                   keys.times[last + 1] - pageStart <= settings_.maxPageSeconds) {
                ++last;
            }
            addPage(track, keys, first, last);
            if (last + 1 >= keys.times.size()) break;
            first = last;  // the boundary key is duplicated at the start of the next page
        }

        track.pageCount = uint32_t(pages_.size()) - track.firstPage;
        tracks_.push_back(track);
    }

private:
    template <typename T>
    void addPage(const CookedTrack& track, const SourceKeys<T>& keys, size_t first,
                 size_t last) {
        CookedPage page;
        page.startTime = keys.times[first];
        page.firstKey = uint32_t(times_.size());
        page.keyCount = uint32_t(last - first + 1);
        const float pageDuration = keys.times[last] - keys.times[first];
        page.timeScale = pageDuration > 0.0f ? pageDuration / CookedClip::kTimeSteps : 0.0f;

        for (size_t i = first; i <= last; ++i) {
            const float local = keys.times[i] - page.startTime;
            times_.push_back(
                page.timeScale > 0.0f
                    ? uint16_t(std::clamp(std::lround(local / page.timeScale), 0l, 65535l))
                    : uint16_t(0));
            if constexpr (std::is_same_v<T, glm::vec3>) {
                const glm::vec3& v = keys.values[i];
                values_.push_back(quantizeComponent(v.x, track.valueMin.x, track.valueScale.x));
                values_.push_back(quantizeComponent(v.y, track.valueMin.y, track.valueScale.y));
                values_.push_back(quantizeComponent(v.z, track.valueMin.z, track.valueScale.z));
            } else {
                uint16_t packed[3];
                CookedClip::quantizeRotation(keys.values[i], packed);
                values_.insert(values_.end(), packed, packed + 3);
            }
        }
        pages_.push_back(page);
    }

    std::vector<CookedTrack>& tracks_;
    std::vector<CookedPage>& pages_;
    std::vector<uint16_t>& times_;
    std::vector<uint16_t>& values_;
    const CookSettings& settings_;
};

// Maximum source/cooked error for a track, measured over a uniform sampling
// (2x the source key density, bounded).
template <typename T>
float measureError(const TypedAnimTrack<T>& source, const CookedClip& cooked,
                   const Rig& rig, uint16_t boneIndex, float duration) {
    constexpr int kMinProbes = 16;
    constexpr int kMaxProbes = 512;
    const int probes =
        std::clamp(int(source.timestamps.size()) * 2, kMinProbes, kMaxProbes);

    LocalPose pose;
    pose.resize(rig.boneCount());
    float maxError = 0.0f;
    Transform expected;
    for (int i = 0; i <= probes; ++i) {
        const float t = duration * float(i) / float(probes);
        expected = rig.bones()[boneIndex].restLocal;
        source.evaluate(t, expected);
        pose.localTransforms[boneIndex] = rig.bones()[boneIndex].restLocal;
        cooked.sample(t, pose);
        const Transform& actual = pose.localTransforms[boneIndex];
        if constexpr (std::is_same_v<T, glm::vec3>) {
            const glm::vec3 a =
                source.target == TrackTarget::Translation ? actual.position : actual.scale;
            const glm::vec3 e =
                source.target == TrackTarget::Translation ? expected.position : expected.scale;
            maxError = std::max(maxError, channelError(a, e));
        } else {
            maxError = std::max(maxError, channelError(actual.rotation, expected.rotation));
        }
    }
    return maxError;
}

template <typename T>
size_t sourceKeyBytes(const TypedAnimTrack<T>& track) {
    return track.timestamps.size() * (sizeof(float) + sizeof(T)) +
           (track.inTangents.size() + track.outTangents.size()) * sizeof(T);
}

template <typename T>
void hashTrack(uint64_t& h, const TypedAnimTrack<T>& track) {
    hashValue(h, track.target);
    hashValue(h, track.interpolation);
    hashBytes(h, track.timestamps.data(), track.timestamps.size() * sizeof(float));
    hashBytes(h, track.values.data(), track.values.size() * sizeof(T));
    hashBytes(h, track.inTangents.data(), track.inTangents.size() * sizeof(T));
    hashBytes(h, track.outTangents.data(), track.outTangents.size() * sizeof(T));
}

float toleranceFor(TrackTarget target, const CookSettings& settings) {
    switch (target) {
        case TrackTarget::Translation: return settings.translationTolerance;
        case TrackTarget::Rotation: return settings.rotationTolerance;
        case TrackTarget::Scale: return settings.scaleTolerance;
    }
    return settings.translationTolerance;
}

} // namespace

uint64_t CookSettings::hash() const {
    uint64_t h = kFnvOffset;
    hashValue(h, translationTolerance);
    hashValue(h, rotationTolerance);
    hashValue(h, scaleTolerance);
    hashValue(h, splineResampleRate);
    hashValue(h, maxPageSeconds);
    return h;
}

bool CookReport::withinTolerance(const CookSettings& settings) const {
    // Quantization adds on top of key reduction: the checked bound is
    // twice the reduction tolerance.
    return maxTranslationError <= 2.0f * settings.translationTolerance &&
           maxRotationError <= 2.0f * settings.rotationTolerance &&
           maxScaleError <= 2.0f * settings.scaleTolerance;
}

nlohmann::json CookReport::toJson() const {
    return {{"clip", clipName},
            {"sourceKeys", sourceKeys},
            {"cookedKeys", cookedKeys},
            {"cookedTracks", cookedTracks},
            {"constantTracks", constantTracks},
            {"restPoseTracksDropped", restPoseTracksDropped},
            {"unmappedTracks", unmappedTracks},
            {"sourceBytes", sourceBytes},
            {"cookedBytes", cookedBytes},
            {"maxTranslationError", maxTranslationError},
            {"maxRotationError", maxRotationError},
            {"maxScaleError", maxScaleError}};
}

CookedClip AnimationCooker::cook(const AnimationClip& clip, const Rig& rig,
                                 const CookSettings& settings, CookReport* report,
                                 const RetargetMap* retarget) {
    CookedClip cooked;
    cooked.name_ = clip.name();
    cooked.duration_ = clip.duration();

    CookReport stats;
    stats.clipName = clip.name();

    // Clip tracks actually consumed (used to count the unmapped ones).
    size_t consumedTrackLists = 0;

    CookedClipBuilder builder(cooked.tracks_, cooked.pages_, cooked.times_,
                              cooked.values_, settings);
    struct MeasuredTrack {
        const AnimTrack* source;
        uint16_t boneIndex;
    };
    std::vector<MeasuredTrack> measured;

    for (size_t boneIndex = 0; boneIndex < rig.boneCount(); ++boneIndex) {
        const Bone& bone = rig.bones()[boneIndex];
        const std::string sourceName =
            retarget ? retarget->resolve(bone.name) : bone.name;
        const auto* tracks = clip.getTracks(sourceName);
        if (!tracks) continue;
        ++consumedTrackLists;

        for (const auto& trackPtr : *tracks) {
            const auto cookTyped = [&](auto* typed) {
                using ValueType = std::remove_const_t<
                    std::remove_reference_t<decltype(typed->values[0])>>;
                if (typed->timestamps.empty()) return;
                stats.sourceKeys += uint32_t(typed->timestamps.size());
                stats.sourceBytes += sourceKeyBytes(*typed);

                const float tolerance = toleranceFor(typed->target, settings);
                SourceKeys<ValueType> keys = extractKeys(*typed, settings);

                if (isConstant(keys, tolerance)) {
                    keys.times.resize(1);
                    keys.values.resize(1);
                    const ValueType rest =
                        restValue<ValueType>(bone.restLocal, typed->target);
                    if (channelError(keys.values.front(), rest) <= tolerance) {
                        ++stats.restPoseTracksDropped;
                        return;
                    }
                    ++stats.constantTracks;
                } else {
                    reduceKeys(keys, typed->interpolation, tolerance);
                }

                builder.addTrack(uint16_t(boneIndex), typed->target,
                                 typed->interpolation, keys);
                stats.cookedKeys += uint32_t(keys.times.size());
                measured.push_back({typed, uint16_t(boneIndex)});
            };

            if (auto* vec = dynamic_cast<const TypedAnimTrack<glm::vec3>*>(trackPtr.get()))
                cookTyped(vec);
            else if (auto* quat = dynamic_cast<const TypedAnimTrack<glm::quat>*>(trackPtr.get()))
                cookTyped(quat);
        }
    }

    stats.cookedTracks = uint32_t(cooked.tracks_.size());
    stats.cookedBytes = cooked.dataBytes();
    stats.unmappedTracks = uint32_t(clip.boneNames().size() - consumedTrackLists);

    for (const MeasuredTrack& m : measured) {
        if (auto* vec = dynamic_cast<const TypedAnimTrack<glm::vec3>*>(m.source)) {
            const float error =
                measureError(*vec, cooked, rig, m.boneIndex, clip.duration());
            if (vec->target == TrackTarget::Translation)
                stats.maxTranslationError = std::max(stats.maxTranslationError, error);
            else
                stats.maxScaleError = std::max(stats.maxScaleError, error);
        } else if (auto* quat = dynamic_cast<const TypedAnimTrack<glm::quat>*>(m.source)) {
            stats.maxRotationError = std::max(
                stats.maxRotationError,
                measureError(*quat, cooked, rig, m.boneIndex, clip.duration()));
        }
    }

    if (report) *report = stats;
    return cooked;
}

uint64_t AnimationCooker::contentHash(const AnimationClip& clip, const Rig& rig,
                                      const CookSettings& settings,
                                      const RetargetMap* retarget) {
    uint64_t h = kFnvOffset;
    hashValue(h, CookedClip::kVersion);
    hashValue(h, kCookerVersion);
    hashValue(h, settings.hash());

    hashString(h, clip.name());
    hashValue(h, clip.duration());
    std::vector<std::string> names = clip.boneNames();
    std::sort(names.begin(), names.end());
    for (const std::string& name : names) {
        hashString(h, name);
        for (const auto& track : *clip.getTracks(name)) {
            if (auto* vec = dynamic_cast<const TypedAnimTrack<glm::vec3>*>(track.get()))
                hashTrack(h, *vec);
            else if (auto* quat = dynamic_cast<const TypedAnimTrack<glm::quat>*>(track.get()))
                hashTrack(h, *quat);
        }
    }

    for (const Bone& bone : rig.bones()) {
        hashString(h, bone.name);
        hashValue(h, bone.parentIndex);
        hashValue(h, bone.restLocal.position);
        hashValue(h, bone.restLocal.rotation);
        hashValue(h, bone.restLocal.scale);
        if (retarget) hashString(h, retarget->resolve(bone.name));
    }
    return h;
}

} // namespace saida
