// Tests du noyau runtime data-oriented : compilation d'un .sgraph en
// AnimationProgram, parité de poses avec le chemin d'authoring, transitions
// par paramètres typés, et absence totale d'allocation dans
// AnimationInstance::update (vérifiée par un hook global operator new).

#include "scene/GLTFLoader.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimationCooker.hpp"
#include "scene/animation/AnimationProgram.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/Rig.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

namespace {

size_t g_allocationCount = 0;

} // namespace

void* operator new(size_t size) {
    ++g_allocationCount;
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, size_t) noexcept { std::free(p); }

namespace {

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::abs(a - b) < tolerance;
}

struct Fixture {
    saida::GltfAnimationData data;
    std::shared_ptr<const saida::CookedClip> cooked;

    Fixture() {
        std::string error;
        const std::string path =
            std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf";
        assert(saida::GLTFLoader::loadAnimationData(path, data, &error));
        cooked = std::make_shared<saida::CookedClip>(saida::AnimationCooker::cook(
            *data.clips[0], *data.rigs[0], saida::CookSettings{}));
    }

    const saida::Rig& rig() const { return *data.rigs[0]; }
};

// Un programme à un clip reproduit l'échantillonnage cuit + le solve global.
void testSingleClipProgram() {
    Fixture fixture;
    auto program = saida::AnimationProgram::singleClip(fixture.cooked);
    assert(program && program->states().size() == 1);

    saida::AnimationInstance instance(program, fixture.rig());
    instance.update(0.5f);
    assert(near(instance.stateTime(), 0.5f));

    saida::LocalPose expected;
    expected.resize(fixture.rig().boneCount());
    for (size_t i = 0; i < fixture.rig().boneCount(); ++i)
        expected.localTransforms[i] = fixture.rig().bones()[i].restLocal;
    fixture.cooked->sample(0.5f, expected);

    for (size_t i = 0; i < fixture.rig().boneCount(); ++i) {
        assert(near(instance.localPose().localTransforms[i].position.x,
                    expected.localTransforms[i].position.x));
        assert(near(instance.localPose().localTransforms[i].position.y,
                    expected.localTransforms[i].position.y));
    }
    // Solve global : l'enfant hérite du parent (mêmes attentes que les golden
    // tests du chemin d'authoring à t=0.5).
    assert(near(instance.globalPose().globalMatrices[1][3].x, 0.5f, 2e-3f));
    assert(near(instance.globalPose().globalMatrices[1][3].y, 1.5f, 2e-3f));

    // La lecture boucle par défaut.
    instance.update(0.75f);
    assert(near(instance.stateTime(), 0.25f));
}

// Le programme compilé depuis un .sgraph transitionne comme la machine d'états
// d'authoring : paramètre typé, crossfade non réévalué, retour d'état.
void testGraphProgramTransitions() {
    Fixture fixture;
    auto parsed = saida::AnimGraphAsset::loadFile(std::string(SAIDA_FIXTURE_DIR) +
                                                  "/animation/locomotion.sgraph");
    assert(parsed.ok);

    std::vector<saida::AssetDiagnostic> diagnostics;
    auto program = saida::AnimationProgram::compile(
        parsed.graph,
        [&](const std::string& key) {
            return key == "two_bone.gltf#Mix" ? fixture.cooked : nullptr;
        },
        &diagnostics);
    assert(program && diagnostics.empty());
    assert(program->states().size() == 2);
    assert(program->stateName(program->initialState()) == "Idle");

    saida::AnimationInstance instance(program, fixture.rig());
    const int speed = program->paramIndex("speed");
    assert(speed >= 0);
    assert(near(instance.param(uint32_t(speed)), 0.0f));  // défaut appliqué

    instance.update(0.016f);
    assert(instance.currentStateName() == "Idle");

    instance.setParam(uint32_t(speed), 1.0f);
    instance.update(0.016f);
    assert(instance.currentStateName() == "Run");

    // Pas de réévaluation pendant le crossfade (0.2 s dans la fixture).
    instance.setParam(uint32_t(speed), 0.0f);
    instance.update(0.016f);
    assert(instance.currentStateName() == "Run");
    instance.update(0.3f);
    instance.update(0.016f);
    assert(instance.currentStateName() == "Idle");

    // Un graphe sans clip résoluble échoue avec diagnostic.
    std::vector<saida::AssetDiagnostic> missing;
    auto none = saida::AnimationProgram::compile(
        parsed.graph, [](const std::string&) { return nullptr; }, &missing);
    assert(!none && !missing.empty());
}

// L'invariant central du kernel : aucune allocation après la construction,
// transitions et crossfades compris.
void testUpdateNeverAllocates() {
    Fixture fixture;
    auto parsed = saida::AnimGraphAsset::loadFile(std::string(SAIDA_FIXTURE_DIR) +
                                                  "/animation/locomotion.sgraph");
    assert(parsed.ok);
    auto program = saida::AnimationProgram::compile(
        parsed.graph,
        [&](const std::string& key) {
            return key == "two_bone.gltf#Mix" ? fixture.cooked : nullptr;
        });
    assert(program);

    saida::AnimationInstance instance(program, fixture.rig());
    const uint32_t speed = uint32_t(program->paramIndex("speed"));
    instance.update(0.016f);  // premier tick : curseurs et états en place

    const size_t before = g_allocationCount;
    for (int frame = 0; frame < 600; ++frame) {
        instance.setParam(speed, (frame / 60) % 2 == 0 ? 0.0f : 1.0f);
        instance.update(1.0f / 60.0f);
    }
    assert(g_allocationCount == before);
}

} // namespace

int main() {
    testSingleClipProgram();
    testGraphProgramTransitions();
    testUpdateNeverAllocates();
    std::puts("saida_animation_instance_tests: OK");
    return 0;
}
