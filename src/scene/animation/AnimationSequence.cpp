#include "scene/animation/AnimationSequence.hpp"

#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipNode.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>

namespace saida {

namespace {

using json = nlohmann::json;

AssetDiagnostic error(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Error, std::move(path),
            std::move(message)};
}

AssetDiagnostic warning(std::string code, std::string path, std::string message) {
    return {std::move(code), AssetDiagnostic::Severity::Warning, std::move(path),
            std::move(message)};
}

bool finiteNumber(const json& j) {
    return j.is_number() && std::isfinite(j.get<double>());
}

bool readFloat(const json& j, const char* key, float& out) {
    if (!j.contains(key)) return true;
    if (!finiteNumber(j[key])) return false;
    out = j[key].get<float>();
    return true;
}

} // namespace

float SequenceClipEntry::weightAt(float time) const {
    if (time < start || time > end()) return 0.0f;
    float weight = 1.0f;
    if (blendIn > 0.0f) weight = std::min(weight, (time - start) / blendIn);
    if (blendOut > 0.0f) weight = std::min(weight, (end() - time) / blendOut);
    return std::clamp(weight, 0.0f, 1.0f);
}

AnimationSequenceParseResult AnimationSequence::parse(const nlohmann::json& j) {
    AnimationSequenceParseResult result;
    auto& diags = result.diagnostics;

    if (!j.is_object()) {
        diags.push_back(error("sequence.root.not_object", "", "document must be a JSON object"));
        return result;
    }
    const int schema = j.value("schema", 0);
    if (schema <= 0) {
        diags.push_back(error("sequence.schema.missing", "/schema",
                              "'schema' must be a positive integer"));
        return result;
    }
    if (schema > kAnimationSequenceSchema) {
        diags.push_back(error("sequence.schema.newer", "/schema",
                              "schema " + std::to_string(schema) + " is newer than supported " +
                                  std::to_string(kAnimationSequenceSchema)));
        return result;
    }

    AnimationSequence& s = result.sequence;
    s.name = j.value("name", "");
    if (!j.contains("duration") || !finiteNumber(j["duration"]) ||
        j["duration"].get<float>() <= 0.0f) {
        diags.push_back(error("sequence.duration.malformed", "/duration",
                              "'duration' must be a positive number"));
        return result;
    }
    s.duration = j["duration"].get<float>();

    if (!j.contains("tracks") || !j["tracks"].is_array()) {
        diags.push_back(error("sequence.tracks.missing", "/tracks",
                              "'tracks' must be an array"));
        return result;
    }

    size_t index = 0;
    for (const json& t : j["tracks"]) {
        const std::string path = "/tracks/" + std::to_string(index++);
        const std::string type = t.is_object() ? t.value("type", "") : "";

        if (type == "animation") {
            SequenceAnimationTrack track;
            track.target = t.value("target", "");
            if (track.target.empty()) {
                diags.push_back(error("sequence.track.no_target", path + "/target",
                                      "animation track needs a 'target'"));
                continue;
            }
            if (!t.contains("clips") || !t["clips"].is_array()) {
                diags.push_back(error("sequence.track.no_clips", path + "/clips",
                                      "animation track needs a 'clips' array"));
                continue;
            }
            size_t clipIndex = 0;
            bool trackOk = true;
            for (const json& c : t["clips"]) {
                const std::string cpath = path + "/clips/" + std::to_string(clipIndex++);
                SequenceClipEntry entry;
                if (!c.is_object() || !c.contains("clip") || !c["clip"].is_string() ||
                    c["clip"].get<std::string>().empty()) {
                    diags.push_back(error("sequence.clip.malformed", cpath,
                                          "clip entry must reference a sub-asset key"));
                    trackOk = false;
                    continue;
                }
                entry.clip = c["clip"].get<std::string>();
                if (!readFloat(c, "start", entry.start) ||
                    !readFloat(c, "duration", entry.duration) ||
                    !readFloat(c, "trimStart", entry.trimStart) ||
                    !readFloat(c, "speed", entry.speed) ||
                    !readFloat(c, "blendIn", entry.blendIn) ||
                    !readFloat(c, "blendOut", entry.blendOut)) {
                    diags.push_back(error("sequence.clip.bad_number", cpath,
                                          "clip entry has a non-finite number"));
                    trackOk = false;
                    continue;
                }
                entry.loop = c.value("loop", false);
                track.clips.push_back(std::move(entry));
            }
            if (trackOk) {
                std::sort(track.clips.begin(), track.clips.end(),
                          [](const SequenceClipEntry& a, const SequenceClipEntry& b) {
                              return a.start < b.start;
                          });
                s.animationTracks.push_back(std::move(track));
            }
        } else if (type == "event") {
            if (!t.contains("events") || !t["events"].is_array()) {
                diags.push_back(error("sequence.track.no_events", path + "/events",
                                      "event track needs an 'events' array"));
                continue;
            }
            for (const json& e : t["events"]) {
                if (!e.is_object() || !e.contains("time") || !finiteNumber(e["time"]) ||
                    !e.contains("name") || !e["name"].is_string()) {
                    diags.push_back(error("sequence.event.malformed", path + "/events",
                                          "event must be {time, name}"));
                    continue;
                }
                s.events.push_back({e["time"].get<float>(), e["name"].get<std::string>()});
            }
        } else if (type == "property") {
            SequencePropertyTrack track;
            track.target = t.value("target", "");
            if (track.target.empty() || !t.contains("keys") || !t["keys"].is_array()) {
                diags.push_back(error("sequence.track.property_malformed", path,
                                      "property track needs 'target' and 'keys'"));
                continue;
            }
            for (const json& k : t["keys"]) {
                if (!k.is_object() || !k.contains("time") || !finiteNumber(k["time"]) ||
                    !k.contains("value")) {
                    diags.push_back(error("sequence.key.malformed", path + "/keys",
                                          "key must be {time, value}"));
                    continue;
                }
                track.keys.push_back({k["time"].get<float>(), k["value"]});
            }
            s.propertyTracks.push_back(std::move(track));
        } else {
            diags.push_back(error("sequence.track.unknown_type", path + "/type",
                                  "unknown track type '" + type + "'"));
        }
    }

    std::sort(s.events.begin(), s.events.end(),
              [](const ClipViewEvent& a, const ClipViewEvent& b) { return a.time < b.time; });

    result.ok = !hasErrors(diags);
    return result;
}

AnimationSequenceParseResult AnimationSequence::loadFile(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        AnimationSequenceParseResult result;
        result.diagnostics.push_back(error("sequence.io.open", "", "cannot open " + path));
        return result;
    }
    json j = json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        AnimationSequenceParseResult result;
        result.diagnostics.push_back(error("sequence.io.json", "", path + " is not valid JSON"));
        return result;
    }
    return parse(j);
}

nlohmann::json AnimationSequence::toJson() const {
    json tracks = json::array();
    for (const auto& track : animationTracks) {
        json clips = json::array();
        for (const auto& c : track.clips) {
            json entry = {{"start", c.start}, {"duration", c.duration}, {"clip", c.clip}};
            if (c.trimStart != 0.0f) entry["trimStart"] = c.trimStart;
            if (c.speed != 1.0f) entry["speed"] = c.speed;
            if (c.loop) entry["loop"] = true;
            if (c.blendIn > 0.0f) entry["blendIn"] = c.blendIn;
            if (c.blendOut > 0.0f) entry["blendOut"] = c.blendOut;
            clips.push_back(std::move(entry));
        }
        tracks.push_back({{"type", "animation"}, {"target", track.target}, {"clips", clips}});
    }
    if (!events.empty()) {
        json list = json::array();
        for (const auto& e : events) list.push_back({{"time", e.time}, {"name", e.name}});
        tracks.push_back({{"type", "event"}, {"events", list}});
    }
    for (const auto& track : propertyTracks) {
        json keys = json::array();
        for (const auto& k : track.keys)
            keys.push_back({{"time", k.time}, {"value", k.value}});
        tracks.push_back({{"type", "property"}, {"target", track.target}, {"keys", keys}});
    }
    return {{"schema", kAnimationSequenceSchema}, {"name", name}, {"duration", duration},
            {"tracks", tracks}};
}

bool AnimationSequence::saveFile(const std::string& path) const {
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return false;
    file << toJson().dump(1) << "\n";
    return file.good();
}

std::vector<AssetDiagnostic> AnimationSequence::validate() const {
    std::vector<AssetDiagnostic> diags;

    for (size_t trackIndex = 0; trackIndex < animationTracks.size(); ++trackIndex) {
        const SequenceAnimationTrack& track = animationTracks[trackIndex];
        for (size_t clipIndex = 0; clipIndex < track.clips.size(); ++clipIndex) {
            const SequenceClipEntry& c = track.clips[clipIndex];
            const std::string path = "/tracks/" + std::to_string(trackIndex) + "/clips/" +
                                     std::to_string(clipIndex);
            if (c.duration <= 0.0f)
                diags.push_back(error("sequence.clip.no_duration", path,
                                      "clip entry needs a positive 'duration'"));
            if (c.start < 0.0f || c.end() > duration + 1e-4f)
                diags.push_back(error("sequence.clip.outside_timeline", path,
                                      "clip [" + std::to_string(c.start) + ", " +
                                          std::to_string(c.end()) +
                                          "] leaves the sequence timeline"));
            if (c.blendIn + c.blendOut > c.duration + 1e-4f)
                diags.push_back(error("sequence.clip.blends_too_long", path,
                                      "blendIn + blendOut exceed the entry duration"));
        }
    }
    for (const ClipViewEvent& e : events) {
        if (e.time < 0.0f || e.time > duration)
            diags.push_back(warning("sequence.event.outside_timeline", "/tracks",
                                    "event '" + e.name + "' is outside the sequence"));
    }
    return diags;
}

SequenceTrackNode::SequenceTrackNode(
    const SequenceAnimationTrack& track,
    const std::function<const AnimationClip*(const std::string&)>& resolveClip,
    const Rig& rig, float sequenceDuration, std::vector<AssetDiagnostic>* diagnostics)
    : duration_(sequenceDuration) {
    for (const SequenceClipEntry& placement : track.clips) {
        const AnimationClip* clip = resolveClip(placement.clip);
        if (!clip) {
            if (diagnostics)
                diagnostics->push_back(error("sequence.build.clip_unresolved", "/tracks",
                                             "'" + placement.clip + "' could not be resolved"));
            continue;
        }
        Entry entry;
        entry.placement = placement;
        entry.node = std::make_unique<ClipNode>(clip, rig);
        entry.node->setLooping(placement.loop);
        entries_.push_back(std::move(entry));
    }
}

void SequenceTrackNode::update(float deltaTime) {
    time_ = std::clamp(time_ + deltaTime, 0.0f, duration_);
}

void SequenceTrackNode::setTime(float time) {
    time_ = std::clamp(time, 0.0f, duration_);
}

float SequenceTrackNode::localClipTime(const SequenceClipEntry& placement,
                                       float clipDuration) const {
    float local = placement.trimStart + (time_ - placement.start) * placement.speed;
    const float available = clipDuration - placement.trimStart;
    if (placement.loop && available > 0.0f) {
        local = placement.trimStart + std::fmod(local - placement.trimStart, available);
        if (local < placement.trimStart) local += available;
    }
    return std::clamp(local, 0.0f, clipDuration);
}

void SequenceTrackNode::evaluate(const LocalPose& bindPose, LocalPose& outPose) const {
    outPose = bindPose;

    // Sequential blend of active entries: the first one applies at full
    // weight (the outgoing clip never falls back to the rest pose), later
    // ones blend by their ramp — the incoming clip's blendIn does the crossfade.
    bool firstActive = true;
    for (const Entry& entry : entries_) {
        float weight = entry.placement.weightAt(time_);
        if (weight <= 0.0f) continue;
        if (firstActive) {
            weight = 1.0f;
            firstActive = false;
        }

        entry.node->setTime(localClipTime(entry.placement, entry.node->duration()));
        if (weight >= 1.0f) {
            entry.node->evaluate(bindPose, outPose);
            continue;
        }
        entry.node->evaluate(bindPose, scratchPose_);
        for (size_t i = 0; i < outPose.localTransforms.size(); ++i) {
            Transform& out = outPose.localTransforms[i];
            const Transform& sampled = scratchPose_.localTransforms[i];
            out.position = glm::mix(out.position, sampled.position, weight);
            out.rotation = glm::slerp(out.rotation, sampled.rotation, weight);
            out.scale = glm::mix(out.scale, sampled.scale, weight);
        }
    }
}

void SequencePlayer::seek(float time) {
    time_ = std::clamp(time, 0.0f, duration_);
    applyTime();
}

void SequencePlayer::applyTime() {
    for (SequenceTrackNode* node : trackNodes_) node->setTime(time_);
    propertyTimeline_.evaluate(time_);
}

void SequencePlayer::update(float deltaTime) {
    const float previous = time_;
    time_ = std::clamp(time_ + deltaTime, 0.0f, duration_);
    applyTime();
    for (const ClipViewEvent& event : events_) {
        if (event.time > previous && event.time <= time_) sequenceEvent.emit(event.name);
    }
}

bool SequencePlayer::bind(const AnimationSequence& sequence,
                          const AnimatorResolver& resolveAnimator,
                          const ClipResolver& resolveClip, const PropertyBinder& bindProperty,
                          std::vector<AssetDiagnostic>* diagnostics) {
    trackNodes_.clear();
    propertyTimeline_ = Timeline();
    events_ = sequence.events;
    duration_ = sequence.duration;
    time_ = 0.0f;

    bool anyBound = false;
    for (const SequenceAnimationTrack& track : sequence.animationTracks) {
        Animator* animator = resolveAnimator(track.target);
        if (!animator || !animator->rig()) {
            if (diagnostics)
                diagnostics->push_back(error("sequence.bind.target_unresolved", "/tracks",
                                             "no animator for target '" + track.target + "'"));
            continue;
        }
        auto node = std::make_unique<SequenceTrackNode>(track, resolveClip,
                                                        *animator->rig(),
                                                        sequence.duration, diagnostics);
        if (node->empty()) continue;
        trackNodes_.push_back(node.get());
        animator->setRootNode(std::move(node));
        anyBound = true;
    }

    for (const SequencePropertyTrack& track : sequence.propertyTracks) {
        auto propertyTrack = std::make_unique<TimelinePropertyTrack>(track.target);
        if (bindProperty && !bindProperty(track.target, *propertyTrack)) {
            if (diagnostics)
                diagnostics->push_back(error("sequence.bind.property_unresolved", "/tracks",
                                             "property '" + track.target + "' left unbound"));
            continue;
        }
        for (const SequencePropertyKey& key : track.keys)
            propertyTrack->addKey(key.time, key.value);
        propertyTimeline_.addTrack(std::move(propertyTrack));
        anyBound = anyBound || bindProperty != nullptr;
    }
    propertyTimeline_.setDuration(sequence.duration);

    return anyBound;
}

} // namespace saida
