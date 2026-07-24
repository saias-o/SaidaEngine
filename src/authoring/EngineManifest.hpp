#pragma once

// Stable engine description for external tools.
// Keeps authoring-core linked into the web build so its WASM size stays real.

#include "core/EngineVersion.hpp"

#include <nlohmann/json.hpp>

namespace saida::authoring {

// { engineVersion, opVersion, nodes:[...], behaviours:[...], ops:[...],
//   scenario:{ actions:[...], conditions:[...] } }
nlohmann::json buildEngineManifest();

// Snapshots materialize the current version of the authoring contract.
constexpr const char* kEngineVersion = ::saida::kEngineVersion;
constexpr int kOpVersion = 2;

} // namespace saida::authoring
