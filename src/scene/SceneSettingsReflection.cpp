// The reflected description of a scene's environment.
//
// `SceneSettings` was the last authored surface in the engine still described
// by hand at every consumer: the Inspector, the `set_scene_setting` op, the MCP
// tool and the serializer each carried their own list of keys, their own idea
// of which ones existed, and — for two of them — their own spelling. Scripts
// would have been the fifth list, which is the mistake the reflected-property
// binding had just removed for nodes.
//
// So the environment is described once, here, and the two writers that had no
// reason to know a field's name in advance — the op applier and the script
// binding — resolve through it. Two consumers deliberately still do not:
//
//   The Inspector, because it does not just write values: it lays out sliders,
//   colour wheels and collapsing sections, and reads the scene's bake state to
//   grey half of them out. Reflection carries the range, the tooltip and the
//   group needed to generate that, and doing so is worth its own change.
//
//   The serializer, because scene files are a durable format: it resolves a
//   skybox by *path* (registry ids are regenerated on rescan), and two of its
//   keys — `ambient` and `postProcessing` — have been written into every scene
//   ever saved under names the ops call `ambientLight` and
//   `enablePostProcessing`. Renaming either would break the format; renaming
//   the op keys would break recorded op streams. The divergence is therefore
//   kept, and `SceneSettingsTests` pins the two lists to each other by value so
//   it cannot grow.
//
// What belongs here is the *authored* environment: exactly the fields a scene
// file carries. `baked` and `bakeRequested` are renderer handshake state,
// `giDebugVoxels` and `showSkeletons` are editor debug toggles; none of them is
// something a scene author or a gameplay script has any business writing, and
// none of them is saved.

#include "core/Reflection.hpp"
#include "scene/Scene.hpp"

namespace saida {

void SceneSettings::describe(reflect::TypeBuilder<SceneSettings>& t) {
    t.doc("A scene's environment: ambient, sky, global illumination, fog and post.");

    // Names match the `set_scene_setting` op, which is a published contract.
    // Where the scene file spells one differently, the serializer keeps the old
    // spelling and the test maps the two.
    t.colorProperty("ambientLight", &SceneSettings::ambientLight)
        .group("Environment")
        .tooltip("linear RGB skylight added to every surface");
    t.colorProperty("clearColor", &SceneSettings::clearColor)
        .group("Environment")
        .tooltip("linear RGB behind the scene, where no skybox is drawn");
    t.property("enablePostProcessing", &SceneSettings::enablePostProcessing)
        .group("Environment");
    t.property("changeRenderingAtLoad", &SceneSettings::changeRenderingAtLoad)
        .group("Environment")
        .tooltip("loading this scene imposes its environment on the World");

    t.property("lightingMode", &SceneSettings::lightingMode)
        .group("Lighting")
        .enumValues({"Realtime", "Baked"});

    t.property("giEnabled", &SceneSettings::giEnabled).group("Global illumination");
    t.property("giMode", &SceneSettings::giMode)
        .group("Global illumination")
        .enumValues({"FullRealtime", "AmortizedRealtime"});
    t.property("giIntensity", &SceneSettings::giIntensity)
        .group("Global illumination").range(0.0, 8.0);

    // `skyboxTexture` is deliberately absent, and it is the one field a scene
    // file carries that this list does not. It is an `AssetID`, and the scene
    // format stores the *path* precisely because ids are regenerated whenever
    // the registry is rescanned. Reflection has no asset-path setter — `asset`
    // is a string kind whose setter would be handed an integer — so exposing it
    // would offer ops and scripts the one form of the reference that does not
    // survive a rescan. Swapping a sky belongs with the asset API.
    t.property("skyboxExposure", &SceneSettings::skyboxExposure)
        .group("Sky").range(0.0, 8.0);
    t.property("skyboxRotation", &SceneSettings::skyboxRotation)
        .group("Sky").range(0.0, 360.0);
    t.property("iblEnabled", &SceneSettings::iblEnabled).group("Sky");
    t.property("iblDiffuseIntensity", &SceneSettings::iblDiffuseIntensity)
        .group("Sky").range(0.0, 4.0);
    t.property("iblSpecularIntensity", &SceneSettings::iblSpecularIntensity)
        .group("Sky").range(0.0, 4.0);

    t.property("aoEnabled", &SceneSettings::aoEnabled).group("Ambient occlusion");
    t.property("aoRadius", &SceneSettings::aoRadius)
        .group("Ambient occlusion").range(0.0, 4.0);
    t.property("aoIntensity", &SceneSettings::aoIntensity)
        .group("Ambient occlusion").range(0.0, 4.0);
    t.property("aoPower", &SceneSettings::aoPower)
        .group("Ambient occlusion").range(0.0, 8.0);

    t.property("fogEnabled", &SceneSettings::fogEnabled).group("Fog");
    t.colorProperty("fogColor", &SceneSettings::fogColor)
        .group("Fog")
        .tooltip("linear RGB the distance fades to");
    t.property("fogStart", &SceneSettings::fogStart)
        .group("Fog").range(0.0, 1000.0)
        .tooltip("metres before fog begins to accumulate");
    t.property("fogDensity", &SceneSettings::fogDensity)
        .group("Fog").range(0.0, 1.0);

    t.property("bloomEnabled", &SceneSettings::bloomEnabled).group("Bloom");
    t.property("bloomThreshold", &SceneSettings::bloomThreshold)
        .group("Bloom").range(0.0, 8.0);
    t.property("bloomIntensity", &SceneSettings::bloomIntensity)
        .group("Bloom").range(0.0, 4.0);
    t.property("bloomRadius", &SceneSettings::bloomRadius)
        .group("Bloom").range(0.0, 16.0);
}

const reflect::TypeDesc& sceneSettingsDesc() {
    // A local descriptor rather than a `TypeRegistry` entry: the registry is a
    // catalogue of things a scene can *contain*, keyed by node and behaviour
    // type name, and `find("SceneSettings")` returning a type that no node
    // factory can build would be a trap for every caller that walks it. The
    // engine manifest publishes the same description under `sceneSettings`, so
    // it stays discoverable without pretending to be a node.
    static const reflect::TypeDesc desc = [] {
        reflect::TypeDesc d = reflect::localDesc<SceneSettings>();
        d.name = "SceneSettings";
        d.category = "settings";
        return d;
    }();
    return desc;
}

} // namespace saida
