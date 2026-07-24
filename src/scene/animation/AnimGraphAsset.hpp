#pragma once

// AnimGraphAsset — the persistent playback-logic asset (.sgraph, JSON).
// The visual editor and LLM tools manipulate this document; the runtime
// never evaluates the JSON directly — build() compiles it into an
// AnimStateMachine, and AnimationProgram::compile() into a data-oriented program.
//
// JSON schema (schema == kAnimGraphSchema):
//   {
//     "schema": 2,
//     "name": "Locomotion",
//     "parameters": [ { "name": "speed", "type": "float", "default": 0 },
//                     { "name": "jump", "type": "trigger" } ],
//     "clips": { "idle": "models/hero.glb#Idle" },   // alias -> sub-asset key
//     "states": [ { "name": "Idle", "play": "idle", "loop": true, "speed": 1.0 } ],
//     "initial": "Idle",
//     "transitions": [
//       { "from": "Idle", "to": "Walk", "crossfade": 0.2, "syncPhase": true,
//         "when": [ { "param": "speed", "op": ">", "value": 0.1 } ] },
//       { "from": "Attack", "to": "Idle", "exitTime": 0.9 }   // one-shot
//     ]
//   }
//
#include "scene/animation/ClipView.hpp"  // AssetDiagnostic

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace saida {

class AnimationClip;
class AnimStateMachine;
class Rig;

constexpr int kAnimGraphSchema = 2;

// A trigger is 1 when armed and goes back to 0 once a transition that
// consumes it is taken.
enum class AnimParamType { Float, Int, Bool, Trigger };

struct AnimGraphParam {
    std::string name;
    AnimParamType type = AnimParamType::Float;
    float defaultValue = 0.0f;
};

struct AnimGraphClipRef {
    std::string alias;  // local name within the graph
    std::string key;    // sub-asset key "file#clip"
};

struct AnimGraphState {
    std::string name;
    std::string play;  // clip alias
    bool loop = true;
    float speed = 1.0f;
};

struct AnimGraphCondition {
    std::string param;
    std::string op;  // "==", "!=", ">", "<", ">=", "<="
    float value = 0.0f;
};

struct AnimGraphTransition {
    std::string from;
    std::string to;
    float crossfade = 0.0f;
    float exitTime = -1.0f;  // minimum normalized exit phase (< 0 = free)
    bool syncPhase = false;  // the target state starts at the source state's phase
    std::vector<AnimGraphCondition> when;  // AND-ed conditions
};

struct AnimGraphParseResult;

class AnimGraphAsset {
public:
    static AnimGraphParseResult parse(const nlohmann::json& j);
    static AnimGraphParseResult loadFile(const std::string& path);

    nlohmann::json toJson() const;
    bool saveFile(const std::string& path) const;

    // Internal consistency: referenced states/aliases/parameters exist,
    // initial is valid, no duplicates, states unreachable from initial (warning).
    std::vector<AssetDiagnostic> validate() const;

    // Compiles into the runtime state machine. `resolveClip` maps a
    // sub-asset key to a loaded clip (null = not found -> diagnostic, state skipped).
    // Returns null if no state could be built.
    std::unique_ptr<AnimStateMachine> build(
        const std::function<const AnimationClip*(const std::string& key)>& resolveClip,
        const Rig& rig, std::vector<AssetDiagnostic>* diagnostics = nullptr) const;

    std::string name;
    std::vector<AnimGraphParam> parameters;
    std::vector<AnimGraphClipRef> clips;
    std::vector<AnimGraphState> states;
    std::string initial;
    std::vector<AnimGraphTransition> transitions;

    const AnimGraphClipRef* findClip(const std::string& alias) const;
    const AnimGraphState* findState(const std::string& stateName) const;
    const AnimGraphParam* findParam(const std::string& paramName) const;
};

struct AnimGraphParseResult {
    bool ok = false;
    AnimGraphAsset graph;
    std::vector<AssetDiagnostic> diagnostics;
};

} // namespace saida
