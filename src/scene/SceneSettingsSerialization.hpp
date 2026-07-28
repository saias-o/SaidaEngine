#pragma once

#include "project/AssetRegistry.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

namespace saida {

struct SceneSettings;
class ResourceManager;

// The one reader/writer of a scene's rendering settings in the runtime `.scene`
// format. Two call sites need it — `Scene::serialize`/`deserialize` for the
// settings embedded in a scene node (prefab overrides included) and
// `SceneSerializer::loadIntoScene` for a scene file — and they used to carry
// their own copy of the field list, which drifted: different defaults, and only
// one of them accepted an asset path where the other demanded a numeric AssetID.
//
// `writeSceneSettings` emits every field, so a document written by the engine is
// always complete. `applySceneSettings` patches: a field absent from `j` leaves
// `out` untouched, which is what a prefab override layered on top of a loaded
// scene requires. A caller that wants "the document alone decides" assigns
// `SceneSettings{}` first.

// How a settings document's asset references reach the project. A scene names an
// asset by path — the durable form, since registry ids are regenerated on every
// rescan — and this is the only thing the reader needs a project for. Naming it
// keeps the reader usable (and testable) without a GPU device behind it.
using AssetPathResolver = std::function<AssetID(const std::string& path, AssetType type)>;

void writeSceneSettings(const SceneSettings& settings, nlohmann::json& out);

void applySceneSettings(const nlohmann::json& j, SceneSettings& out,
                        const AssetPathResolver& resolve);
void applySceneSettings(const nlohmann::json& j, SceneSettings& out,
                        ResourceManager& resources);

} // namespace saida
