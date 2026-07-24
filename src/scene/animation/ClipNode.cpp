#include "scene/animation/ClipNode.hpp"

#include <algorithm>
#include <cmath>

namespace saida {

ClipNode::ClipNode(const AnimationClip* clip, const Rig& rig, const RetargetMap* retarget)
    : clip_(clip) {
    if (!clip_) return;

    // Bind tracks to rig bones (O(1) mapping setup). With a retarget map, the rig
    // bone name is translated to the clip's track name first.
    boundTracks_.resize(rig.boneCount(), nullptr);
    for (size_t i = 0; i < rig.boneCount(); ++i) {
        const std::string& rigName = rig.bones()[i].name;
        const std::string& clipName = retarget ? retarget->resolve(rigName) : rigName;
        boundTracks_[i] = clip_->getTracks(clipName);
    }
}

void ClipNode::setRange(float start, float end) {
    rangeStart_ = start;
    rangeEnd_ = end;
}

float ClipNode::rangeEnd() const {
    return rangeEnd_ >= 0.0f ? rangeEnd_ : duration();
}

void ClipNode::update(float deltaTime) {
    if (!clip_) return;

    const float previousTime = time_;
    const float advance = deltaTime * speed_;
    const bool forward = advance >= 0.0f;
    time_ += advance;

    // Loop or clamp inside the playable window ([0, duration] or the view range).
    const float start = rangeStart_;
    const float end = rangeEnd();
    const float span = end - start;
    bool wrapped = false;
    if (span > 0.0f) {
        if (looping_) {
            // fmod maps `end` exactly onto `start`: reaching the end while
            // going forward therefore counts as a wrap, reaching the start
            // while going backward does not. (Events and root motion assume
            // at most one wrap per update — a dt longer than a loop would
            // lose one.)
            wrapped = forward ? time_ >= end : time_ < start;
            time_ = start + std::fmod(time_ - start, span);
            if (time_ < start) {
                time_ += span;
            }
        } else {
            if (time_ > end) time_ = end;
            if (time_ < start) time_ = start;
        }
    } else {
        time_ = start;
    }

    firedEvents_.clear();
    if (!events_.empty()) {
        if (!wrapped) {
            collectCrossings(previousTime, time_);
        } else if (forward) {
            collectCrossings(previousTime, end);
            collectCrossings(start, time_);
        } else {
            collectCrossings(previousTime, start);
            collectCrossings(end, time_);
        }
    }

    if (rootMotionBone_ >= 0) {
        if (!wrapped) {
            rootMotionDelta_ += rootTranslationAt(time_) - rootTranslationAt(previousTime);
        } else if (forward) {
            rootMotionDelta_ += (rootTranslationAt(end) - rootTranslationAt(previousTime)) +
                                (rootTranslationAt(time_) - rootTranslationAt(start));
        } else {
            rootMotionDelta_ += (rootTranslationAt(start) - rootTranslationAt(previousTime)) +
                                (rootTranslationAt(time_) - rootTranslationAt(end));
        }
    }
}

// Events crossed between `from` and `to`: interval ]from, to] going forward,
// [to, from[ going backward.
void ClipNode::collectCrossings(float from, float to) {
    if (from == to) return;
    const bool forward = to > from;
    for (uint32_t i = 0; i < events_.size(); ++i) {
        const float t = events_[i].time;
        const bool crossed = forward ? (t > from && t <= to) : (t >= to && t < from);
        if (crossed) firedEvents_.push_back(i);
    }
}

glm::vec3 ClipNode::rootTranslationAt(float time) const {
    Transform transform;
    if (rootMotionBone_ >= 0 && size_t(rootMotionBone_) < boundTracks_.size() &&
        boundTracks_[size_t(rootMotionBone_)]) {
        for (const auto& track : *boundTracks_[size_t(rootMotionBone_)])
            track->evaluate(time, transform);
    }
    return transform.position;
}

void ClipNode::setEvents(std::vector<ClipViewEvent> events) {
    events_ = std::move(events);
    std::sort(events_.begin(), events_.end(),
              [](const ClipViewEvent& a, const ClipViewEvent& b) { return a.time < b.time; });
}

void ClipNode::setRootMotionBone(int32_t boneIndex) {
    rootMotionBone_ = boneIndex;
    rootMotionDelta_ = glm::vec3(0.0f);
}

glm::vec3 ClipNode::takeRootMotionDelta() {
    const glm::vec3 delta = rootMotionDelta_;
    rootMotionDelta_ = glm::vec3(0.0f);
    return delta;
}

float ClipNode::normalizedTime() const {
    const float span = rangeEnd() - rangeStart_;
    if (span <= 0.0f) return 0.0f;
    return (time_ - rangeStart_) / span;
}

void ClipNode::seekNormalized(float phase) {
    const float span = rangeEnd() - rangeStart_;
    time_ = rangeStart_ + std::clamp(phase, 0.0f, 1.0f) * std::max(span, 0.0f);
}

void ClipNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    outPose.resize(bindPose.localTransforms.size());

    for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
        outPose.localTransforms[i] = bindPose.localTransforms[i];

        if (i < boundTracks_.size() && boundTracks_[i] != nullptr) {
            for (const auto& track : *boundTracks_[i]) {
                track->evaluate(time_, outPose.localTransforms[i]);
            }
        }
    }

    // The extracted displacement is consumed by gameplay: the pose keeps the
    // root at its rest translation so it isn't applied twice.
    if (rootMotionBone_ >= 0 && size_t(rootMotionBone_) < outPose.localTransforms.size()) {
        outPose.localTransforms[size_t(rootMotionBone_)].position =
            bindPose.localTransforms[size_t(rootMotionBone_)].position;
    }
}

} // namespace saida
