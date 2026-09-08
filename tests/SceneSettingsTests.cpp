// Locks the scene environment contract: what a `.scene` file carries, and what
// an op or a script may write.
//
// The format half used to be read by two hand-copied field lists that had
// drifted apart: different defaults on the same key, and a skybox nameable by
// path in one of them and only by numeric AssetID in the other — where a path
// is the durable form, since registry ids are regenerated on every rescan.
//
// The authoring half is `sceneSettingsDesc()`, a description of the same struct
// that cannot be merged with the format one: its keys are the op vocabulary,
// not the file's, and it deliberately omits both the skybox reference and the
// engine's own transient state. Two descriptions of one struct drift, so the
// second half of this file makes them prove each other — every reflected
// setting must survive a save/load, and every saved key must stay reachable.

#include "core/Reflection.hpp"
#include "scene/Scene.hpp"
#include "scene/SceneSettingsSerialization.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <cmath>
#include <set>
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

// ── the reflected description, and its agreement with the format ────────────

// Reflection and the scene format spell two settings differently. Neither name
// can move: the file names have been written into every scene ever saved, and
// the reflected names are the `set_scene_setting` op's published vocabulary.
// The mapping is declared here, once, and this is the only place in the engine
// allowed to know about it.
const char* serializedKeyFor(const std::string& reflected) {
    if (reflected == "ambientLight") return "ambient";
    if (reflected == "enablePostProcessing") return "postProcessing";
    return nullptr;  // same name on both sides
}

// The one field a scene carries that reflection deliberately does not describe:
// an AssetID whose durable form is a path, which reflection has no setter for.
const std::set<std::string> kSerializedOnly = {"skyboxTexture"};

// A legal value that is not the current one, so a round-trip that drops the
// field fails instead of being accidentally right.
json distinctValue(const saida::reflect::PropertyDesc& prop, const json& current) {
    if (prop.kind == "bool") return !current.get<bool>();
    if (prop.kind == "float") return current.get<double>() + 0.375;
    if (prop.kind == "int") return current.get<long long>() + 3;
    if (prop.kind == "enum") {
        const int count = static_cast<int>(prop.enumLabels.size());
        assert(count > 0 && "an enum setting must declare its labels");
        return (current.get<int>() + 1) % count;
    }
    if (prop.kind == "vec3")
        return json::array({current[0].get<double>() + 0.125,
                            current[1].get<double>() + 0.25,
                            current[2].get<double>() + 0.375});
    assert(false && "a scene setting gained a kind this test does not cover");
    return json();
}

// Everything an op or a script can write is saved with the scene. Without this,
// a setting could be reflected, changed by a designer, and lost on reload.
void testEveryReflectedSettingRoundTrips() {
    const saida::reflect::TypeDesc& desc = saida::sceneSettingsDesc();
    assert(!desc.properties.empty());

    for (const auto& prop : desc.properties) {
        saida::SceneSettings source;
        json current;
        prop.get(&source, current);
        prop.set(&source, distinctValue(prop, current));

        // What the field now holds, not what was asked for: a float cannot
        // represent every double, and the invariant under test is that the
        // *stored* value survives the file, not that the test picked a value
        // the mantissa happens to like.
        json wanted;
        prop.get(&source, wanted);
        assert(wanted != current && "distinctValue must move the field");

        json written;
        saida::writeSceneSettings(source, written);

        const char* alias = serializedKeyFor(prop.name);
        const std::string key = alias ? alias : prop.name;
        assert(written.contains(key) &&
               "a reflected scene setting is never written to the scene file");

        saida::SceneSettings restored;
        saida::applySceneSettings(written, restored,
                                  [](const std::string&, saida::AssetType) {
                                      return saida::kAssetInvalid;
                                  });

        json readBack;
        prop.get(&restored, readBack);
        assert(readBack == wanted &&
               "a reflected scene setting does not survive a save/load round-trip");
    }
}

// And the converse: nothing a scene carries is unreachable by accident. A field
// added to the serializer alone fails here rather than becoming an environment
// no tool can edit.
void testEverySerializedKeyIsReflected() {
    json written;
    saida::writeSceneSettings(saida::SceneSettings{}, written);

    const saida::reflect::TypeDesc& desc = saida::sceneSettingsDesc();
    for (const auto& entry : written.items()) {
        const std::string key = entry.key();
        if (kSerializedOnly.count(key)) continue;
        bool found = desc.findProperty(key) != nullptr;
        for (const auto& prop : desc.properties) {
            if (found) break;
            const char* alias = serializedKeyFor(prop.name);
            found = alias && key == alias;
        }
        assert(found && "a scene file key has no reflected setting");
    }
}

// Colours carry an alpha the renderer never reads. Reading gives three
// channels; writing three leaves the fourth exactly where it was — the very
// disagreement that produced this file's first half.
void testReflectedColoursKeepAlpha() {
    const saida::reflect::PropertyDesc* fog =
        saida::sceneSettingsDesc().findProperty("fogColor");
    assert(fog && fog->kind == "vec3");

    saida::SceneSettings s;
    s.fogColor = {0.1f, 0.2f, 0.3f, 0.75f};

    json read;
    fog->get(&s, read);
    assert(read.is_array() && read.size() == 3);
    assert(near(read[0].get<float>(), 0.1f));

    fog->set(&s, json::array({0.4f, 0.5f, 0.6f}));
    assert(near(s.fogColor.r, 0.4f) && near(s.fogColor.b, 0.6f));
    assert(near(s.fogColor.a, 0.75f));

    // The kind check is what lets a writer report a refusal: the setter would
    // ignore a malformed value and leave the caller believing it had applied.
    std::string why;
    assert(!saida::reflect::valueMatchesKind(*fog, json::array({0.4f, 0.5f}), why));
    assert(!saida::reflect::valueMatchesKind(*fog, 0.4f, why));
    fog->set(&s, json::array({0.9f, 0.9f}));
    assert(near(s.fogColor.r, 0.4f));
}

// Enum settings declare their labels, so a value outside them is out of range
// rather than a rendering mode nothing implements.
void testReflectedEnumsAreRangeChecked() {
    const saida::reflect::PropertyDesc* mode =
        saida::sceneSettingsDesc().findProperty("giMode");
    assert(mode && mode->kind == "enum" && mode->enumLabels.size() == 2);

    std::string why;
    assert(saida::reflect::valueMatchesKind(*mode, 1, why));
    assert(!saida::reflect::valueMatchesKind(*mode, 2, why));
    assert(!saida::reflect::valueMatchesKind(*mode, -1, why));
    assert(!saida::reflect::valueMatchesKind(*mode, 0.5, why));

    saida::SceneSettings s;
    mode->set(&s, 1);
    assert(s.giMode == saida::GIMode::AmortizedRealtime);
}

// The renderer's bake handshake and the editor's debug toggles are engine
// state, not authored environment. They are not saved, and no author writes
// them; reflection must not offer them either.
void testInternalStateIsNotReflected() {
    const saida::reflect::TypeDesc& desc = saida::sceneSettingsDesc();
    for (const char* name : {"baked", "bakeRequested", "giDebugVoxels", "showSkeletons"})
        assert(desc.findProperty(name) == nullptr);
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
    testEveryReflectedSettingRoundTrips();
    testEverySerializedKeyIsReflected();
    testReflectedColoursKeepAlpha();
    testReflectedEnumsAreRangeChecked();
    testInternalStateIsNotReflected();
    return 0;
}
