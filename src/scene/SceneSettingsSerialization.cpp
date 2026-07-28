#include "scene/SceneSettingsSerialization.hpp"

#include "graphics/ResourceManager.hpp"
#include "scene/Scene.hpp"
#include "scene/SerializationHelpers.hpp"

#include <nlohmann/json.hpp>

namespace saida {

namespace {

using nlohmann::json;

// Resolves an asset reference that may be written either as a registry-relative
// path or as a raw AssetID. The path form is the durable one: registry ids are
// regenerated whenever the registry is rescanned, so a scene that pastes an id
// silently loses its reference, while a path survives.
AssetID readAssetRef(const json& j, AssetType type, const AssetPathResolver& resolve,
                     AssetID fallback) {
    if (j.is_string()) {
        const std::string path = j.get<std::string>();
        // An explicitly empty path means "no asset", not "keep the old one".
        if (path.empty()) return kAssetInvalid;
        return resolve ? resolve(path, type) : kAssetInvalid;
    }
    if (j.is_number_integer()) return j.get<AssetID>();
    return fallback;
}

template <typename T>
void readInto(const json& j, const char* key, T& out) {
    if (auto it = j.find(key); it != j.end()) out = it->get<T>();
}

template <typename Enum>
void readEnumInto(const json& j, const char* key, Enum& out) {
    if (auto it = j.find(key); it != j.end())
        out = static_cast<Enum>(it->get<int>());
}

void readVec3Into(const json& j, const char* key, glm::vec4& out) {
    if (auto it = j.find(key); it != j.end())
        out = glm::vec4(jsonToVec3(*it, glm::vec3(out)), out.w);
}

} // namespace

void writeSceneSettings(const SceneSettings& s, json& out) {
    out = json{
        {"ambient", vec3ToJson(s.ambientLight)},
        {"clearColor", vec3ToJson(s.clearColor)},
        {"postProcessing", s.enablePostProcessing},
        {"lightingMode", static_cast<int>(s.lightingMode)},
        {"giEnabled", s.giEnabled},
        {"giMode", static_cast<int>(s.giMode)},
        {"giIntensity", s.giIntensity},
        {"skyboxTexture", s.skyboxTexture},
        {"skyboxExposure", s.skyboxExposure},
        {"skyboxRotation", s.skyboxRotation},
        {"iblEnabled", s.iblEnabled},
        {"iblDiffuseIntensity", s.iblDiffuseIntensity},
        {"iblSpecularIntensity", s.iblSpecularIntensity},
        {"aoEnabled", s.aoEnabled},
        {"aoRadius", s.aoRadius},
        {"aoIntensity", s.aoIntensity},
        {"aoPower", s.aoPower},
        {"fogEnabled", s.fogEnabled},
        {"fogColor", vec3ToJson(s.fogColor)},
        {"fogStart", s.fogStart},
        {"fogDensity", s.fogDensity},
        {"bloomEnabled", s.bloomEnabled},
        {"bloomThreshold", s.bloomThreshold},
        {"bloomIntensity", s.bloomIntensity},
        {"bloomRadius", s.bloomRadius},
        {"changeRenderingAtLoad", s.changeRenderingAtLoad},
    };
}

void applySceneSettings(const json& j, SceneSettings& out, ResourceManager& resources) {
    applySceneSettings(j, out, [&resources](const std::string& path, AssetType type) {
        return resources.getOrRegister(path, type);
    });
}

void applySceneSettings(const json& j, SceneSettings& out, const AssetPathResolver& resolve) {
    if (!j.is_object()) return;

    // The colours keep their alpha: only .rgb reaches the shader, and rewriting
    // the whole vec4 is how the two former readers ended up disagreeing on it.
    readVec3Into(j, "ambient", out.ambientLight);
    readVec3Into(j, "clearColor", out.clearColor);
    readVec3Into(j, "fogColor", out.fogColor);

    readInto(j, "postProcessing", out.enablePostProcessing);
    readEnumInto(j, "lightingMode", out.lightingMode);
    readInto(j, "giEnabled", out.giEnabled);
    readEnumInto(j, "giMode", out.giMode);
    readInto(j, "giIntensity", out.giIntensity);

    if (auto it = j.find("skyboxTexture"); it != j.end())
        out.skyboxTexture = readAssetRef(*it, AssetType::Texture, resolve, out.skyboxTexture);
    readInto(j, "skyboxExposure", out.skyboxExposure);
    readInto(j, "skyboxRotation", out.skyboxRotation);

    readInto(j, "iblEnabled", out.iblEnabled);
    readInto(j, "iblDiffuseIntensity", out.iblDiffuseIntensity);
    readInto(j, "iblSpecularIntensity", out.iblSpecularIntensity);

    readInto(j, "aoEnabled", out.aoEnabled);
    readInto(j, "aoRadius", out.aoRadius);
    readInto(j, "aoIntensity", out.aoIntensity);
    readInto(j, "aoPower", out.aoPower);

    readInto(j, "fogEnabled", out.fogEnabled);
    readInto(j, "fogStart", out.fogStart);
    readInto(j, "fogDensity", out.fogDensity);

    readInto(j, "bloomEnabled", out.bloomEnabled);
    readInto(j, "bloomThreshold", out.bloomThreshold);
    readInto(j, "bloomIntensity", out.bloomIntensity);
    readInto(j, "bloomRadius", out.bloomRadius);

    readInto(j, "changeRenderingAtLoad", out.changeRenderingAtLoad);
}

} // namespace saida
