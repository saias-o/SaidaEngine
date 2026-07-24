// Headless WASM build of the engine authoring-core validation. Loaded
// in-process by the Node
// Collaboration Gateway so it validates incoming SaidaOps with the *real* C++
// contract (parseSaidaOp + validateOpShape) — no TypeScript reimplementation
// (zero duplication) and no per-op subprocess (no latency). Extends invariant
// 0.2 (authoring-core compiled into the edit runtime) to the server.
//
// This links ONLY src/authoring/SaidaOp.cpp: op schema/version/known-type +
// per-type shape validation, with no scene, no reflection, no GPU. The heavier
// EngineManifest (reflected properties, behaviours, scenario) stays in the
// native saida_tool describe-engine, consumed by the gateway as a cached blob.

#include "authoring/EngineManifest.hpp"  // kOpVersion (header-only constant)
#include "authoring/SaidaOp.hpp"

#include <emscripten.h>
#include <nlohmann/json.hpp>

#include <string>

using nlohmann::json;

namespace {
// Returned strings live in a single static buffer: each call overwrites the
// previous result. The Node caller copies the string out (ccall 'string')
// before the next call — fine on the single-threaded event loop.
const char* hold(std::string value) {
    static std::string buffer;
    buffer = std::move(value);
    return buffer.c_str();
}
}  // namespace

extern "C" {

// Statically validate one SaidaOp (JSON string). Returns a JSON report:
//   {"ok":true,"type":"set_transform","opVersion":2}
//   {"ok":false,"error":"set_transform needs a decimal string 'nodeId'"}
// Never throws; malformed JSON is reported as a normal validation failure.
EMSCRIPTEN_KEEPALIVE
const char* saida_validate_op(const char* opJson) {
    const auto parsed = saida::authoring::parseSaidaOp(std::string(opJson ? opJson : ""));
    const std::string error =
        parsed.ok ? saida::authoring::validateOpShape(parsed.op) : parsed.error;
    json report{{"ok", error.empty()}};
    if (error.empty()) {
        report["type"] = parsed.op.type;
        report["opVersion"] = parsed.op.opVersion;
    } else {
        report["error"] = error;
    }
    return hold(report.dump());
}

// The op contract types the engine understands (source of truth for the UI/IA).
EMSCRIPTEN_KEEPALIVE
const char* saida_known_op_types() {
    return hold(json(saida::authoring::knownOpTypes()).dump());
}

// The op-log schema version the engine speaks (intra-version — invariant 0.6).
EMSCRIPTEN_KEEPALIVE
int saida_op_version() {
    return saida::authoring::kOpVersion;
}

}  // extern "C"
