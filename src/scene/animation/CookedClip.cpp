#include "scene/animation/CookedClip.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace saida {

namespace {

constexpr float kQuatComponentMax = 0.70710678f;  // 1/sqrt(2)
constexpr uint16_t kQuatComponentBits = 15;
constexpr uint16_t kQuatComponentMask = (1u << kQuatComponentBits) - 1;
constexpr float kQuatComponentSteps = float(kQuatComponentMask);
// Beyond this cosine, two keys are close enough for a normalized nlerp
// (indistinguishable from slerp) — the short path is guaranteed either way.
constexpr float kNlerpDotThreshold = 0.9995f;

glm::quat blendRotations(const glm::quat& a, glm::quat b, float t) {
    float d = glm::dot(a, b);
    if (d < 0.0f) {
        b = -b;
        d = -d;
    }
    if (d > kNlerpDotThreshold)
        return glm::normalize(a * (1.0f - t) + b * t);  // nlerp (component-wise lerp)
    return glm::slerp(a, b, t);
}

uint16_t quantizeQuatComponent(float v) {
    const float normalized = (v + kQuatComponentMax) / (2.0f * kQuatComponentMax);
    const float clamped = std::clamp(normalized, 0.0f, 1.0f);
    return uint16_t(std::lround(clamped * kQuatComponentSteps));
}

float dequantizeQuatComponent(uint16_t q) {
    return (float(q) / kQuatComponentSteps) * 2.0f * kQuatComponentMax - kQuatComponentMax;
}

template <typename Writer>
void writeBytes(std::vector<uint8_t>& out, const Writer& value) {
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), raw, raw + sizeof(Writer));
}

template <typename T>
bool readBytes(const uint8_t*& cursor, const uint8_t* end, T& out) {
    if (cursor + sizeof(T) > end) return false;
    std::memcpy(&out, cursor, sizeof(T));
    cursor += sizeof(T);
    return true;
}

template <typename T>
bool readVector(const uint8_t*& cursor, const uint8_t* end, std::vector<T>& out) {
    uint32_t count = 0;
    if (!readBytes(cursor, end, count)) return false;
    const size_t bytes = size_t(count) * sizeof(T);
    if (cursor + bytes > end) return false;
    out.resize(count);
    if (bytes > 0) std::memcpy(out.data(), cursor, bytes);
    cursor += bytes;
    return true;
}

template <typename T>
void writeVector(std::vector<uint8_t>& out, const std::vector<T>& values) {
    writeBytes(out, uint32_t(values.size()));
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(values.data());
    out.insert(out.end(), raw, raw + values.size() * sizeof(T));
}

} // namespace

void CookedClip::quantizeRotation(const glm::quat& q, uint16_t out[3]) {
    const float components[4] = {q.x, q.y, q.z, q.w};
    uint32_t largest = 0;
    for (uint32_t i = 1; i < 4; ++i)
        if (std::abs(components[i]) > std::abs(components[largest])) largest = i;

    const float sign = components[largest] < 0.0f ? -1.0f : 1.0f;
    uint16_t packed[3];
    uint32_t slot = 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (i == largest) continue;
        packed[slot++] = quantizeQuatComponent(components[i] * sign);
    }
    out[0] = packed[0] | uint16_t((largest & 1u) << kQuatComponentBits);
    out[1] = packed[1] | uint16_t(((largest >> 1) & 1u) << kQuatComponentBits);
    out[2] = packed[2];
}

glm::quat CookedClip::dequantizeRotation(const uint16_t in[3]) {
    const uint32_t largest = uint32_t(in[0] >> kQuatComponentBits) |
                             (uint32_t(in[1] >> kQuatComponentBits) << 1);
    const float a = dequantizeQuatComponent(uint16_t(in[0] & kQuatComponentMask));
    const float b = dequantizeQuatComponent(uint16_t(in[1] & kQuatComponentMask));
    const float c = dequantizeQuatComponent(in[2]);
    // The reconstructed component makes the quaternion unit by construction:
    // no extra normalization needed. a/b/c follow the x,y,z,w order minus
    // the largest component.
    const float d = std::sqrt(std::max(0.0f, 1.0f - (a * a + b * b + c * c)));
    switch (largest) {
        case 0: return glm::quat(c, d, a, b);
        case 1: return glm::quat(c, a, d, b);
        case 2: return glm::quat(c, a, b, d);
        default: return glm::quat(d, a, b, c);
    }
}

float CookedClip::keyTime(const CookedPage& page, uint32_t key) const {
    return page.startTime + float(times_[key]) * page.timeScale;
}

glm::vec3 CookedClip::keyVec3(const CookedTrack& track, uint32_t key) const {
    const uint16_t* raw = &values_[size_t(key) * kValuesPerKey];
    return track.valueMin + glm::vec3(float(raw[0]), float(raw[1]), float(raw[2])) *
                                track.valueScale;
}

glm::quat CookedClip::keyQuat(uint32_t key) const {
    return dequantizeRotation(&values_[size_t(key) * kValuesPerKey]);
}

void CookedClip::sampleTrack(const CookedTrack& track, float time,
                             CookedCursor& cursor, Transform& out) const {
    // Page covering `time`: the last page whose first key is <= time.
    // The cursor resumes from the previous page during forward playback.
    if (cursor.page < track.firstPage || cursor.page >= track.firstPage + track.pageCount ||
        pages_[cursor.page].startTime > time) {
        cursor.page = track.firstPage;
        cursor.key = pages_[track.firstPage].firstKey;
    }
    while (cursor.page + 1 < track.firstPage + track.pageCount &&
           pages_[cursor.page + 1].startTime <= time) {
        ++cursor.page;
        cursor.key = pages_[cursor.page].firstKey;
    }
    const CookedPage& page = pages_[cursor.page];

    uint32_t prev = page.firstKey;
    uint32_t next = prev;
    if (page.keyCount > 1 && time > page.startTime) {
        if (cursor.key < page.firstKey || cursor.key >= page.firstKey + page.keyCount ||
            keyTime(page, cursor.key) > time) {
            cursor.key = page.firstKey;
        }
        while (cursor.key + 1 < page.firstKey + page.keyCount &&
               keyTime(page, cursor.key + 1) <= time) {
            ++cursor.key;
        }
        prev = cursor.key;
        next = std::min(prev + 1, page.firstKey + page.keyCount - 1);
    }

    float factor = 0.0f;
    if (next != prev && track.interpolation != TrackInterpolation::Step) {
        const float t0 = keyTime(page, prev);
        const float t1 = keyTime(page, next);
        factor = t1 > t0 ? std::clamp((time - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
    }

    if (track.target == TrackTarget::Rotation) {
        const glm::quat a = keyQuat(prev);
        out.rotation = factor > 0.0f ? blendRotations(a, keyQuat(next), factor) : a;
    } else {
        const glm::vec3 a = keyVec3(track, prev);
        const glm::vec3 v = factor > 0.0f ? glm::mix(a, keyVec3(track, next), factor) : a;
        if (track.target == TrackTarget::Translation) out.position = v;
        else out.scale = v;
    }
}

void CookedClip::sample(float time, LocalPose& outPose, CookedCursor* cursors) const {
    CookedCursor scratch;
    for (size_t i = 0; i < tracks_.size(); ++i) {
        const CookedTrack& track = tracks_[i];
        if (track.boneIndex >= outPose.localTransforms.size()) continue;
        CookedCursor& cursor = cursors ? cursors[i] : scratch;
        if (!cursors) scratch = CookedCursor{};
        sampleTrack(track, time, cursor, outPose.localTransforms[track.boneIndex]);
    }
}

size_t CookedClip::dataBytes() const {
    return tracks_.size() * sizeof(CookedTrack) + pages_.size() * sizeof(CookedPage) +
           times_.size() * sizeof(uint16_t) + values_.size() * sizeof(uint16_t);
}

std::vector<uint8_t> CookedClip::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(64 + dataBytes());
    writeBytes(out, kMagic);
    writeBytes(out, kVersion);
    writeBytes(out, uint32_t(name_.size()));
    out.insert(out.end(), name_.begin(), name_.end());
    writeBytes(out, duration_);
    writeVector(out, tracks_);
    writeVector(out, pages_);
    writeVector(out, times_);
    writeVector(out, values_);
    return out;
}

bool CookedClip::deserialize(const uint8_t* data, size_t size, CookedClip& out,
                             std::string* error) {
    const uint8_t* cursor = data;
    const uint8_t* end = data + size;
    const auto fail = [&](const char* message) {
        if (error) *error = message;
        return false;
    };

    uint32_t magic = 0, version = 0;
    if (!readBytes(cursor, end, magic) || magic != kMagic)
        return fail("not a cooked animation clip");
    if (!readBytes(cursor, end, version) || version != kVersion)
        return fail("unsupported cooked clip version");

    uint32_t nameLength = 0;
    if (!readBytes(cursor, end, nameLength) || cursor + nameLength > end)
        return fail("truncated cooked clip");
    out.name_.assign(reinterpret_cast<const char*>(cursor), nameLength);
    cursor += nameLength;

    if (!readBytes(cursor, end, out.duration_) ||
        !readVector(cursor, end, out.tracks_) || !readVector(cursor, end, out.pages_) ||
        !readVector(cursor, end, out.times_) || !readVector(cursor, end, out.values_))
        return fail("truncated cooked clip");

    if (out.values_.size() != out.times_.size() * kValuesPerKey)
        return fail("inconsistent cooked key streams");
    for (const CookedTrack& track : out.tracks_) {
        if (size_t(track.firstPage) + track.pageCount > out.pages_.size())
            return fail("cooked track references pages out of range");
    }
    for (const CookedPage& page : out.pages_) {
        if (size_t(page.firstKey) + page.keyCount > out.times_.size() || page.keyCount == 0)
            return fail("cooked page references keys out of range");
    }
    return true;
}

} // namespace saida
