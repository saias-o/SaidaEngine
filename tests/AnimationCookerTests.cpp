// Tests du pipeline de cuisson animation : fidélité source/cuit sous
// tolérance, canaux constants, suppression des pistes rest pose, réduction de
// clés, pagination des longues pistes, curseurs de lecture et cache disque.

#include "scene/GLTFLoader.hpp"
#include "scene/animation/AnimationCache.hpp"
#include "scene/animation/AnimationCooker.hpp"
#include "scene/animation/CookedClip.hpp"
#include "scene/animation/Rig.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace {

bool near(float a, float b, float tolerance = 1e-5f) {
    return std::abs(a - b) < tolerance;
}

saida::Rig makeChainRig(int boneCount) {
    saida::Rig rig;
    for (int i = 0; i < boneCount; ++i) {
        saida::Transform rest;
        rest.position = {0.0f, 1.0f, 0.0f};
        rig.addBone("bone_" + std::to_string(i), i - 1, glm::mat4(1.0f), rest);
    }
    std::string error;
    assert(rig.finalize(&error));
    return rig;
}

saida::LocalPose makeBindPose(const saida::Rig& rig) {
    saida::LocalPose pose;
    pose.resize(rig.boneCount());
    for (size_t i = 0; i < rig.boneCount(); ++i)
        pose.localTransforms[i] = rig.bones()[i].restLocal;
    return pose;
}

void testQuatQuantizationRoundtrip() {
    for (int i = 0; i < 64; ++i) {
        const float angle = float(i) * 0.31f;
        const glm::vec3 axis = glm::normalize(
            glm::vec3(std::sin(float(i)), std::cos(float(i) * 0.7f), 0.5f));
        const glm::quat q = glm::angleAxis(angle, axis);

        uint16_t packed[3];
        saida::CookedClip::quantizeRotation(q, packed);
        glm::quat restored = saida::CookedClip::dequantizeRotation(packed);
        if (glm::dot(q, restored) < 0.0f) restored = -restored;

        // Pas de quantification : 1.4142/32767 ≈ 4.3e-5 par composante.
        assert(std::abs(q.x - restored.x) < 1e-4f);
        assert(std::abs(q.y - restored.y) < 1e-4f);
        assert(std::abs(q.z - restored.z) < 1e-4f);
        assert(std::abs(q.w - restored.w) < 1e-4f);
    }
}

// La fixture glTF (LINEAR + STEP + CUBICSPLINE) cuite reproduit la source sous
// tolérance sur un balayage dense — le golden test source/cuit du plan.
void testCookGltfFixture() {
    saida::GltfAnimationData data;
    std::string error;
    const std::string path = std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf";
    assert(saida::GLTFLoader::loadAnimationData(path, data, &error));
    const saida::Rig& rig = *data.rigs[0];
    const saida::AnimationClip& source = *data.clips[0];

    saida::CookSettings settings;
    saida::CookReport report;
    const saida::CookedClip cooked = saida::AnimationCooker::cook(source, rig, settings, &report);

    assert(cooked.name() == source.name());
    assert(near(cooked.duration(), source.duration()));
    assert(report.cookedTracks > 0);
    assert(report.withinTolerance(settings));

    const saida::LocalPose bind = makeBindPose(rig);
    for (int probe = 0; probe <= 200; ++probe) {
        const float t = source.duration() * float(probe) / 200.0f;

        saida::LocalPose expected = bind;
        for (size_t b = 0; b < rig.boneCount(); ++b) {
            if (const auto* tracks = source.getTracks(rig.bones()[b].name)) {
                for (const auto& track : *tracks)
                    track->evaluate(t, expected.localTransforms[b]);
            }
        }

        saida::LocalPose actual = bind;
        cooked.sample(t, actual);
        for (size_t b = 0; b < rig.boneCount(); ++b) {
            const auto& e = expected.localTransforms[b];
            const auto& a = actual.localTransforms[b];
            assert(near(a.position.x, e.position.x, 2e-3f));
            assert(near(a.position.y, e.position.y, 2e-3f));
            assert(near(a.position.z, e.position.z, 2e-3f));
            const float dot = std::min(1.0f, std::abs(glm::dot(a.rotation, e.rotation)));
            assert(2.0f * std::acos(dot) < 4e-3f);
        }
    }
}

// Canal constant → une clé ; canal égal à la rest pose → piste supprimée.
void testConstantAndRestPoseTracks() {
    saida::Rig rig = makeChainRig(2);
    saida::AnimationClip clip("synthetic", 1.0f);

    auto constant = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    constant->target = saida::TrackTarget::Translation;
    constant->timestamps = {0.0f, 0.5f, 1.0f};
    constant->values = {{2.0f, 3.0f, 4.0f}, {2.0f, 3.0f, 4.0f}, {2.0f, 3.0f, 4.0f}};
    clip.addTrack("bone_0", std::move(constant));

    auto rest = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    rest->target = saida::TrackTarget::Translation;
    rest->timestamps = {0.0f, 1.0f};
    rest->values = {{0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}};
    clip.addTrack("bone_1", std::move(rest));

    saida::CookReport report;
    const saida::CookedClip cooked =
        saida::AnimationCooker::cook(clip, rig, saida::CookSettings{}, &report);

    assert(report.constantTracks == 1);
    assert(report.restPoseTracksDropped == 1);
    assert(cooked.trackCount() == 1);
    assert(cooked.keyCount() == 1);

    saida::LocalPose pose = makeBindPose(rig);
    cooked.sample(0.7f, pose);
    assert(near(pose.localTransforms[0].position.x, 2.0f, 1e-3f));
    assert(near(pose.localTransforms[0].position.y, 3.0f, 1e-3f));
    assert(near(pose.localTransforms[1].position.y, 1.0f));  // rest inchangée
}

// Une rampe linéaire dense se réduit à ses extrémités sans perte.
void testKeyReduction() {
    saida::Rig rig = makeChainRig(1);
    saida::AnimationClip clip("ramp", 1.0f);

    auto ramp = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    ramp->target = saida::TrackTarget::Translation;
    for (int i = 0; i <= 100; ++i) {
        const float t = float(i) / 100.0f;
        ramp->timestamps.push_back(t);
        ramp->values.push_back({10.0f * t, 1.0f, 0.0f});
    }
    clip.addTrack("bone_0", std::move(ramp));

    saida::CookReport report;
    const saida::CookedClip cooked =
        saida::AnimationCooker::cook(clip, rig, saida::CookSettings{}, &report);

    assert(report.sourceKeys == 101);
    assert(report.cookedKeys == 2);
    assert(report.cookedBytes < report.sourceBytes);

    saida::LocalPose pose = makeBindPose(rig);
    cooked.sample(0.35f, pose);
    assert(near(pose.localTransforms[0].position.x, 3.5f, 2e-3f));
}

// Les pistes STEP cuites tiennent leur valeur jusqu'à la clé suivante.
void testStepTrackCooked() {
    saida::Rig rig = makeChainRig(1);
    saida::AnimationClip clip("step", 1.0f);

    auto step = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    step->target = saida::TrackTarget::Translation;
    step->interpolation = saida::TrackInterpolation::Step;
    step->timestamps = {0.0f, 0.5f, 1.0f};
    step->values = {{0.0f, 1.0f, 0.0f}, {5.0f, 1.0f, 0.0f}, {9.0f, 1.0f, 0.0f}};
    clip.addTrack("bone_0", std::move(step));

    const saida::CookedClip cooked =
        saida::AnimationCooker::cook(clip, rig, saida::CookSettings{});

    saida::LocalPose pose = makeBindPose(rig);
    cooked.sample(0.49f, pose);
    assert(near(pose.localTransforms[0].position.x, 0.0f, 1e-3f));
    cooked.sample(0.51f, pose);
    assert(near(pose.localTransforms[0].position.x, 5.0f, 1e-3f));
}

// Une piste plus longue que maxPageSeconds est paginée et reste continue à la
// frontière (la clé frontière est dupliquée en tête de page suivante).
void testLongTrackPagination() {
    saida::Rig rig = makeChainRig(1);
    const float duration = 150.0f;
    saida::AnimationClip clip("long", duration);

    auto track = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    track->target = saida::TrackTarget::Translation;
    for (int i = 0; i <= 300; ++i) {
        const float t = duration * float(i) / 300.0f;
        track->timestamps.push_back(t);
        track->values.push_back({std::sin(t * 0.1f) * 5.0f, 1.0f, 0.0f});
    }
    const saida::TypedAnimTrack<glm::vec3> reference = *track;
    clip.addTrack("bone_0", std::move(track));

    saida::CookSettings settings;
    settings.maxPageSeconds = 60.0f;
    saida::CookReport report;
    const saida::CookedClip cooked =
        saida::AnimationCooker::cook(clip, rig, settings, &report);

    assert(cooked.pageCount() >= 3);
    assert(report.withinTolerance(settings));

    saida::LocalPose pose = makeBindPose(rig);
    saida::Transform expected;
    for (float t : {59.9f, 60.0f, 60.1f, 119.9f, 120.1f, 149.9f}) {
        cooked.sample(t, pose);
        expected = rig.bones()[0].restLocal;
        reference.evaluate(t, expected);
        assert(near(pose.localTransforms[0].position.x, expected.position.x, 2e-3f));
    }
}

// Lecture avant avec curseurs == échantillonnage sans curseurs (mêmes poses),
// y compris après un retour arrière (seek).
void testCursorsMatchStatelessSampling() {
    saida::GltfAnimationData data;
    std::string error;
    assert(saida::GLTFLoader::loadAnimationData(
        std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf", data, &error));
    const saida::Rig& rig = *data.rigs[0];
    const saida::CookedClip cooked =
        saida::AnimationCooker::cook(*data.clips[0], rig, saida::CookSettings{});

    std::vector<saida::CookedCursor> cursors(cooked.trackCount());
    const saida::LocalPose bind = makeBindPose(rig);
    const float sweep[] = {0.0f, 0.1f, 0.3f, 0.55f, 0.8f, 1.0f, 0.2f, 0.9f};
    for (float t : sweep) {
        saida::LocalPose withCursors = bind;
        saida::LocalPose stateless = bind;
        cooked.sample(t, withCursors, cursors.data());
        cooked.sample(t, stateless);
        for (size_t b = 0; b < rig.boneCount(); ++b) {
            assert(near(withCursors.localTransforms[b].position.y,
                        stateless.localTransforms[b].position.y));
            assert(near(withCursors.localTransforms[b].rotation.z,
                        stateless.localTransforms[b].rotation.z));
        }
    }
}

// Sérialisation binaire : roundtrip exact, corruption refusée proprement.
void testSerializationRoundtrip() {
    saida::GltfAnimationData data;
    std::string error;
    assert(saida::GLTFLoader::loadAnimationData(
        std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf", data, &error));
    const saida::Rig& rig = *data.rigs[0];
    const saida::CookedClip cooked =
        saida::AnimationCooker::cook(*data.clips[0], rig, saida::CookSettings{});

    const std::vector<uint8_t> bytes = cooked.serialize();
    saida::CookedClip restored;
    assert(saida::CookedClip::deserialize(bytes.data(), bytes.size(), restored));
    assert(restored.serialize() == bytes);
    assert(restored.name() == cooked.name());
    assert(restored.trackCount() == cooked.trackCount());

    saida::CookedClip rejected;
    assert(!saida::CookedClip::deserialize(bytes.data(), bytes.size() / 2, rejected));
    std::vector<uint8_t> corrupted = bytes;
    corrupted[0] ^= 0xFF;
    assert(!saida::CookedClip::deserialize(corrupted.data(), corrupted.size(), rejected));
}

// Cache disque : cuisson au premier accès, relecture ensuite, invalidation
// quand les réglages changent.
void testDiskCache() {
    saida::GltfAnimationData data;
    std::string error;
    assert(saida::GLTFLoader::loadAnimationData(
        std::string(SAIDA_FIXTURE_DIR) + "/animation/two_bone.gltf", data, &error));
    const saida::Rig& rig = *data.rigs[0];
    const saida::AnimationClip& clip = *data.clips[0];

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "saida_anim_cache_test";
    std::filesystem::remove_all(dir);
    saida::AnimationCache cache(dir.string());

    saida::CookSettings settings;
    const auto first = cache.getOrCook(clip, rig, settings);
    assert(first.clip && !first.fromCache);
    const auto second = cache.getOrCook(clip, rig, settings);
    assert(second.clip && second.fromCache);
    assert(second.clip->serialize() == first.clip->serialize());

    saida::CookSettings looser = settings;
    looser.translationTolerance *= 10.0f;
    const auto recooked = cache.getOrCook(clip, rig, looser);
    assert(!recooked.fromCache);
    assert(saida::AnimationCooker::contentHash(clip, rig, looser) !=
           saida::AnimationCooker::contentHash(clip, rig, settings));

    std::filesystem::remove_all(dir);
}

} // namespace

int main() {
    testQuatQuantizationRoundtrip();
    testCookGltfFixture();
    testConstantAndRestPoseTracks();
    testKeyReduction();
    testStepTrackCooked();
    testLongTrackPagination();
    testCursorsMatchStatelessSampling();
    testSerializationRoundtrip();
    testDiskCache();
    std::puts("saida_animation_cooker_tests: OK");
    return 0;
}
