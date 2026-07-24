#pragma once

// ClipView — non-destructive view of a source animation.
// An .sclip file describes a trim/loop/speed of an imported clip without
// ever duplicating its keys: several views (Idle, Walk, RunStart...) share
// the same source AnimationClip.
//
// JSON schema (schema == kClipViewSchema):
//   {
//     "schema": 1,
//     "source": "models/mocap.glb#Take1",   // sub-asset key (file#clip)
//     "name": "RunLoop",
//     "range": { "start": 1.2, "end": 2.05 },  // optional: defaults to the whole clip
//     "loop": true,
//     "speed": 1.0,
//     "events": [ { "time": 0.18, "name": "left_foot_down" } ]
//   }

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace saida {

class AnimationClip;
class ClipNode;
class Rig;

// Structured diagnostic for an authoring asset:
// stable code for tooling, JSON path pointing at the offending field,
// human/LLM-readable message.
struct AssetDiagnostic {
    enum class Severity { Error, Warning };

    std::string code;      // e.g. "clipview.range.reversed"
    Severity severity = Severity::Error;
    std::string jsonPath;  // e.g. "/range/end"
    std::string message;

    nlohmann::json toJson() const;
};

bool hasErrors(const std::vector<AssetDiagnostic>& diagnostics);

constexpr int kClipViewSchema = 1;

struct ClipViewEvent {
    float time = 0.0f;  // seconds, in the SOURCE's time space
    std::string name;
};

struct ClipViewParseResult;

class ClipView {
public:
    // Strict parse: invalid JSON or a different schema produces diagnostics
    // and ok=false.
    static ClipViewParseResult parse(const nlohmann::json& j);
    static ClipViewParseResult loadFile(const std::string& path);

    nlohmann::json toJson() const;
    bool saveFile(const std::string& path) const;

    // Validation against the resolved source (durations, ranges, events).
    // Usable without a source (nullptr): only checks internal consistency then.
    std::vector<AssetDiagnostic> validate(const AnimationClip* source) const;

    // Builds the playback node: the ClipNode references the shared source
    // (no key copy) and applies range/speed/loop.
    std::unique_ptr<ClipNode> instantiate(const AnimationClip& source, const Rig& rig) const;

    // Effective range once resolved against the source (defaults filled in).
    float effectiveStart() const { return hasRange ? rangeStart : 0.0f; }
    float effectiveEnd(const AnimationClip& source) const;

    std::string source;  // sub-asset key "file#clip"
    std::string name;
    bool hasRange = false;
    float rangeStart = 0.0f;
    float rangeEnd = 0.0f;
    bool loop = true;
    float speed = 1.0f;
    std::vector<ClipViewEvent> events;

};

struct ClipViewParseResult {
    bool ok = false;
    ClipView view;
    std::vector<AssetDiagnostic> diagnostics;
};

} // namespace saida
