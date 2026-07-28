// Locks the runtime `.scene` rendering-settings contract, which used to be read
// by two hand-copied field lists that had drifted apart: different defaults on
// the same key, and a skybox nameable by path in one of them and only by numeric
// AssetID in the other — where a path is the durable form, since registry ids are
// regenerated on every rescan.

#include "scene/Scene.hpp"
#include "scene/SceneSettingsSerialization.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cmath>
#include <string>

namespace {

using nlohmann::json;

constexpr saida::AssetID kResolvedSky = 990001;

// Stands in for the project: records what the reader asked it to resolve.
struct RecordingResolver {
    mutable std::string lastPath;
    mutable saida::AssetType lastType = saida::AssetType::Unknown;
    mutable int calls = 0;

    saida::AssetPathResolver fn() const {
        return [this](const std::string& path, saida::AssetType type) {
            lastPath = path;
            lastType = type;
            calls += 1;
            return kResolvedSky;
        };
    }
};

bool near(float a, float b) { return std::abs(a - b) < 1e-5f; }

// Settings with every field moved off its default, so a dropped field shows up.
saida::SceneSettings distinctive() {
    saida::SceneSettings s;
    s.ambientLight = {0.11f, 0.22f, 0.33f, 1.0f};
    s.clearColor = {0.44f, 0.55f, 0.66f, 1.0f};
    s.enablePostProcessing = false;
    s.lightingMode = saida::LightingMode::Baked;
    s.giEnabled = false;
    s.giMode = saida::GIMode::AmortizedRealtime;
    s.giIntensity = 2.5f;
    s.skyboxTexture = 4242;
    s.skyboxExposure = 1.75f;
    s.skyboxRotation = 42.0f;
    s.iblEnabled = false;
    s.iblDiffuseIntensity = 0.9f;
    s.iblSpecularIntensity = 0.2f;
    s.aoEnabled = false;
    s.aoRadius = 1.25f;
    s.aoIntensity = 0.4f;
    s.aoPower = 2.5f;
    s.fogEnabled = true;
    s.fogColor = {0.77f, 0.66f, 0.55f, 1.0f};
    s.fogStart = 31.0f;
    s.fogDensity = 0.099f;
    s.bloomEnabled = false;
    s.bloomThreshold = 1.9f;
    s.bloomIntensity = 0.66f;
    s.bloomRadius = 5.5f;
    s.changeRenderingAtLoad = false;
    return s;
}

void expectEqual(const saida::SceneSettings& a, const saida::SceneSettings& b) {
    assert(near(a.ambientLight.r, b.ambientLight.r));
    assert(near(a.ambientLight.g, b.ambientLight.g));
    assert(near(a.ambientLight.b, b.ambientLight.b));
    assert(near(a.clearColor.r, b.clearColor.r));
    assert(near(a.clearColor.g, b.clearColor.g));
    assert(near(a.clearColor.b, b.clearColor.b));
    assert(a.enablePostProcessing == b.enablePostProcessing);
    assert(a.lightingMode == b.lightingMode);
    assert(a.giEnabled == b.giEnabled);
    assert(a.giMode == b.giMode);
    assert(near(a.giIntensity, b.giIntensity));
    assert(a.skyboxTexture == b.skyboxTexture);
    assert(near(a.skyboxExposure, b.skyboxExposure));
    assert(near(a.skyboxRotation, b.skyboxRotation));
    assert(a.iblEnabled == b.iblEnabled);
    assert(near(a.iblDiffuseIntensity, b.iblDiffuseIntensity));
    assert(near(a.iblSpecularIntensity, b.iblSpecularIntensity));
    assert(a.aoEnabled == b.aoEnabled);
    assert(near(a.aoRadius, b.aoRadius));
    assert(near(a.aoIntensity, b.aoIntensity));
    assert(near(a.aoPower, b.aoPower));
    assert(a.fogEnabled == b.fogEnabled);
    assert(near(a.fogColor.r, b.fogColor.r));
    assert(near(a.fogColor.g, b.fogColor.g));
    assert(near(a.fogColor.b, b.fogColor.b));
    assert(near(a.fogStart, b.fogStart));
    assert(near(a.fogDensity, b.fogDensity));
    assert(a.bloomEnabled == b.bloomEnabled);
    assert(near(a.bloomThreshold, b.bloomThreshold));
    assert(near(a.bloomIntensity, b.bloomIntensity));
    assert(near(a.bloomRadius, b.bloomRadius));
    assert(a.changeRenderingAtLoad == b.changeRenderingAtLoad);
}

// Every field written survives being read back — a field forgotten by either
// half of the pair is a silently lost scene setting.
void testRoundTripKeepsEveryField() {
    const saida::SceneSettings source = distinctive();
    json doc;
    saida::writeSceneSettings(source, doc);

    RecordingResolver resolver;
    saida::SceneSettings restored;
    saida::applySceneSettings(doc, restored, resolver.fn());

    expectEqual(source, restored);
    // A numeric id is already resolved; the project must not be consulted.
    assert(resolver.calls == 0);
}

// The regression that motivated the merge: a scene naming its skybox by path
// used to load in one reader and abort the whole scene in the other.
void testSkyboxByPathResolvesThroughTheProject() {
    RecordingResolver resolver;
    saida::SceneSettings settings;
    saida::applySceneSettings(json{{"skyboxTexture", "assets/skies/sky.hdr"}},
                              settings, resolver.fn());

    assert(settings.skyboxTexture == kResolvedSky);
    assert(resolver.calls == 1);
    assert(resolver.lastPath == "assets/skies/sky.hdr");
    assert(resolver.lastType == saida::AssetType::Texture);
}

void testSkyboxByIdIsKeptVerbatim() {
    RecordingResolver resolver;
    saida::SceneSettings settings;
    saida::applySceneSettings(json{{"skyboxTexture", 7777}}, settings, resolver.fn());

    assert(settings.skyboxTexture == 7777);
    assert(resolver.calls == 0);
}

// An empty path is a deliberate "no skybox", not a reason to keep the old one.
void testEmptySkyboxPathClearsTheReference() {
    RecordingResolver resolver;
    saida::SceneSettings settings;
    settings.skyboxTexture = 1234;
    saida::applySceneSettings(json{{"skyboxTexture", ""}}, settings, resolver.fn());

    assert(settings.skyboxTexture == saida::kAssetInvalid);
    assert(resolver.calls == 0);
}

// Patch semantics: this is what lets a prefab instance override one setting
// without restating — and thereby resetting — all the others.
void testAbsentFieldsAreLeftUntouched() {
    RecordingResolver resolver;
    saida::SceneSettings settings = distinctive();
    const saida::SceneSettings before = settings;

    saida::applySceneSettings(json{{"bloomIntensity", 0.125f}}, settings, resolver.fn());

    assert(near(settings.bloomIntensity, 0.125f));
    saida::SceneSettings expected = before;
    expected.bloomIntensity = 0.125f;
    expectEqual(expected, settings);
}

// A colour is written as rgb; reading it must not silently rewrite the alpha,
// which is exactly how the two former readers ended up disagreeing (1.0 vs 0.0).
void testColourAlphaSurvivesAReadback() {
    RecordingResolver resolver;
    saida::SceneSettings settings;
    const float alphaBefore = settings.ambientLight.a;
    saida::applySceneSettings(json{{"ambient", json::array({0.5f, 0.5f, 0.5f})}},
                              settings, resolver.fn());

    assert(near(settings.ambientLight.r, 0.5f));
    assert(near(settings.ambientLight.a, alphaBefore));
}

// A malformed settings block is inert rather than half-applied.
void testNonObjectDocumentIsIgnored() {
    RecordingResolver resolver;
    saida::SceneSettings settings = distinctive();
    const saida::SceneSettings before = settings;
    saida::applySceneSettings(json::array({1, 2, 3}), settings, resolver.fn());
    expectEqual(before, settings);
}

} // namespace

int main() {
    testRoundTripKeepsEveryField();
    testSkyboxByPathResolvesThroughTheProject();
    testSkyboxByIdIsKeptVerbatim();
    testEmptySkyboxPathClearsTheReference();
    testAbsentFieldsAreLeftUntouched();
    testColourAlphaSurvivesAReadback();
    testNonObjectDocumentIsIgnored();
    return 0;
}
