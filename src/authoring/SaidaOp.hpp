#pragma once

// Versioned scene mutation, shared by the editor and the headless tools.
// JSON schema:
//   {
//     "opVersion": 2,
//     "type": "set_transform",
//     "sceneId": "main",
//     "payload": { ... }
//   }

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace saida::authoring {

struct SaidaOp {
    int opVersion = 0;
    std::string type;        // e.g. "set_transform"
    std::string sceneId;     // empty = current scene
    nlohmann::json payload;  // always an object after parsing

    // Omits sceneId when it designates the current scene.
    nlohmann::json toJson() const;
};

struct SaidaOpParseResult {
    bool ok = false;
    SaidaOp op;
    std::string error;  // stable message, empty if ok
};

// Validates the schema, not the scene state.
SaidaOpParseResult parseSaidaOp(const nlohmann::json& j);
SaidaOpParseResult parseSaidaOp(const std::string& text);

// Single source of truth for the manifest and the applier.
const std::vector<std::string>& knownOpTypes();
bool isKnownOpType(const std::string& type);

// Static payload validation; the applier then validates the scene state.
std::string validateOpShape(const SaidaOp& op);

} // namespace saida::authoring
