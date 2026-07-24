// Tests des séquences .sseq : roundtrip et validation du schéma, lecture
// déterministe (seek), crossfade entre deux clips qui se chevauchent,
// événements émis en lecture seulement, piste de propriété réfléchie, et
// SequenceDirectorBehaviour (résolution des cibles par nom dans la scène,
// relais des signaux, fail-closed sur séquence invalide ou cible manquante).

#include "core/Paths.hpp"
#include "nodes/LightNode.hpp"
#include "scene/Node.hpp"
#include "scene/ReflectedTypes.hpp"
#include "behaviours/RotatorBehaviour.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/AnimationSequence.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/Rig.hpp"
#include "scene/animation/SequenceDirectorBehaviour.hpp"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::abs(a - b) < tolerance;
}

bool hasDiagnostic(const std::vector<saida::AssetDiagnostic>& diags, const char* code) {
    for (const auto& d : diags)
        if (d.code == code) return true;
    return false;
}

saida::Rig makeRig() {
    saida::Rig rig;
    saida::Transform rest;
    rest.position = {0.0f, 1.0f, 0.0f};
    rig.addBone("root", -1, glm::mat4(1.0f), rest);
    std::string error;
    assert(rig.finalize(&error));
    return rig;
}

// Clip 1 s : translation X du root de `from` à `to`.
std::unique_ptr<saida::AnimationClip> makeClip(const char* name, float from, float to) {
    auto clip = std::make_unique<saida::AnimationClip>(name, 1.0f);
    auto track = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
    track->target = saida::TrackTarget::Translation;
    track->timestamps = {0.0f, 1.0f};
    track->values = {{from, 1.0f, 0.0f}, {to, 1.0f, 0.0f}};
    clip->addTrack("root", std::move(track));
    return clip;
}

saida::AnimationSequence makeSequence() {
    saida::AnimationSequence sequence;
    sequence.name = "Intro";
    sequence.duration = 3.0f;

    saida::SequenceAnimationTrack track;
    track.target = "Hero";
    saida::SequenceClipEntry first;
    first.start = 0.0f;
    first.duration = 1.5f;
    first.clip = "synthetic#walk";
    first.blendOut = 0.5f;
    saida::SequenceClipEntry second;
    second.start = 1.0f;
    second.duration = 2.0f;
    second.clip = "synthetic#pose";
    second.blendIn = 0.5f;
    track.clips = {first, second};
    sequence.animationTracks.push_back(std::move(track));

    sequence.events.push_back({1.25f, "handoff"});

    saida::SequencePropertyTrack property;
    property.target = "rotator.speed";
    property.keys.push_back({0.0f, 0.0f});
    property.keys.push_back({3.0f, 6.0f});
    sequence.propertyTracks.push_back(std::move(property));
    return sequence;
}

void testRoundtripAndValidation() {
    const saida::AnimationSequence sequence = makeSequence();
    assert(sequence.validate().empty());

    auto parsed = saida::AnimationSequence::parse(sequence.toJson());
    assert(parsed.ok);
    assert(parsed.sequence.animationTracks.size() == 1);
    assert(parsed.sequence.animationTracks[0].clips.size() == 2);
    assert(near(parsed.sequence.animationTracks[0].clips[0].blendOut, 0.5f));
    assert(parsed.sequence.events.size() == 1);
    assert(parsed.sequence.propertyTracks.size() == 1);

    // Schéma plus récent refusé ; clip hors timeline signalé.
    nlohmann::json future = sequence.toJson();
    future["schema"] = saida::kAnimationSequenceSchema + 1;
    assert(!saida::AnimationSequence::parse(future).ok);

    saida::AnimationSequence broken = sequence;
    broken.animationTracks[0].clips[1].duration = 5.0f;
    assert(hasDiagnostic(broken.validate(), "sequence.clip.outside_timeline"));
}

void testDeterministicPlayback() {
    saida::Rig rig = makeRig();
    auto walk = makeClip("walk", 0.0f, 2.0f);
    auto pose = makeClip("pose", 5.0f, 5.0f);

    saida::Animator animator;
    animator.setRig(&rig);

    saida::RotatorBehaviour rotator;

    const saida::AnimationSequence sequence = makeSequence();
    saida::SequencePlayer player;
    std::vector<saida::AssetDiagnostic> diags;
    const bool bound = player.bind(
        sequence,
        [&](const std::string& target) { return target == "Hero" ? &animator : nullptr; },
        [&](const std::string& key) -> const saida::AnimationClip* {
            if (key == "synthetic#walk") return walk.get();
            if (key == "synthetic#pose") return pose.get();
            return nullptr;
        },
        [&](const std::string& target, saida::TimelinePropertyTrack& track) {
            if (target != "rotator.speed") return false;
            track.bind(rotator, "speed");
            return true;
        },
        &diags);
    assert(bound && diags.empty());

    // Seek déterministe : t=0.5 → walk seul (x = 1.0).
    std::vector<std::string> fired;
    saida::Connection events =
        player.sequenceEvent.connect([&](const std::string& name) { fired.push_back(name); });

    player.seek(0.5f);
    animator.onUpdate(0.0f);
    assert(near(animator.globalPose().globalMatrices[0][3].x, 1.0f, 1e-3f));
    assert(fired.empty());  // un seek n'émet pas

    // t=1.25 : milieu du chevauchement — walk (x=2.5→clampé 2.0) pèse 0.5,
    // pose (x=5) pèse 0.5 → blend séquentiel : mix(2.0, 5.0, 0.5) = 3.5.
    player.seek(1.25f);
    animator.onUpdate(0.0f);
    assert(near(animator.globalPose().globalMatrices[0][3].x, 3.5f, 1e-3f));

    // Deux seeks au même temps donnent la même pose (déterminisme).
    player.seek(0.5f);
    player.seek(1.25f);
    animator.onUpdate(0.0f);
    assert(near(animator.globalPose().globalMatrices[0][3].x, 3.5f, 1e-3f));

    // Lecture : l'événement à 1.25 s est émis une fois, la propriété suit.
    player.seek(0.0f);
    player.update(1.5f);
    assert(fired.size() == 1 && fired[0] == "handoff");
    assert(near(rotator.speed, 3.0f, 1e-3f));  // rampe 0→6 sur 3 s, à t=1.5

    player.update(10.0f);  // clampe à la fin
    assert(player.finished());
    assert(near(rotator.speed, 6.0f, 1e-3f));
}

// Projet temporaire minimal pour SequenceDirectorBehaviour : la racine active
// pointe sur un dossier contenant le .sseq écrit par le test.
struct TempProject {
    std::filesystem::path root;

    explicit TempProject(const char* name) {
        root = std::filesystem::temp_directory_path() / name;
        std::filesystem::create_directories(root);
        saida::setActiveProjectRoot(root.string());
    }
    ~TempProject() {
        saida::setActiveProjectRoot("");
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }
    void write(const char* file, const nlohmann::json& j) const {
        std::ofstream out(root / file, std::ios::trunc);
        out << j.dump(1) << "\n";
    }
};

nlohmann::json directorSequenceJson(const char* animTarget) {
    return {
        {"schema", saida::kAnimationSequenceSchema},
        {"name", "Intro"},
        {"duration", 2.0f},
        {"tracks", nlohmann::json::array({
            {{"type", "animation"}, {"target", animTarget}, {"clips", nlohmann::json::array({
                {{"start", 0.0f}, {"duration", 2.0f}, {"clip", "synthetic#walk"}}})}},
            {{"type", "event"}, {"events", nlohmann::json::array({
                {{"time", 0.75f}, {"name", "beat"}}})}},
            {{"type", "property"}, {"target", "Sun.intensity"}, {"keys", nlohmann::json::array({
                {{"time", 0.0f}, {"value", 3.0f}}, {{"time", 2.0f}, {"value", 1.0f}}})}}})}};
}

void testDirectorPlaysSceneSequence() {
    TempProject project("saida_seq_director_test");
    project.write("intro.sseq", directorSequenceJson("Hero"));

    saida::Rig rig = makeRig();
    auto walk = makeClip("walk", 0.0f, 2.0f);

    saida::Node root("Root");
    saida::Node* hero = root.addChild(std::make_unique<saida::Node>("Hero"));
    auto* animator = hero->addBehaviour<saida::Animator>();
    animator->setRig(&rig);
    animator->addClip("walk", walk.get());

    auto* sun = static_cast<saida::LightNode*>(
        root.addChild(std::make_unique<saida::LightNode>()));
    sun->setName("Sun");
    sun->intensity = 3.0f;

    saida::Node* stage = root.addChild(std::make_unique<saida::Node>("Stage"));
    auto* director = stage->addBehaviour<saida::SequenceDirectorBehaviour>();
    director->sequence = "intro.sseq";
    director->autoplay = true;

    std::vector<std::string> fired;
    int finishedCount = 0;
    saida::Connection onEvent = director->sequenceEvent.connect(
        [&](std::string name) { fired.push_back(std::move(name)); });
    saida::Connection onFinished =
        director->sequenceFinished.connect([&]() { ++finishedCount; });

    for (int i = 0; i < 4; ++i) director->onUpdate(0.5f);

    assert(fired.size() == 1 && fired[0] == "beat");
    assert(finishedCount == 1);
    assert(near(sun->intensity, 1.0f, 1e-3f));  // rampe 3→1 sur la durée
    animator->onUpdate(0.0f);
    assert(near(animator->globalPose().globalMatrices[0][3].x, 2.0f, 1e-3f));

    // La lecture terminée n'émet plus rien ; play() rejoue depuis le début.
    director->onUpdate(0.5f);
    assert(finishedCount == 1);
    director->play();
    for (int i = 0; i < 4; ++i) director->onUpdate(0.5f);
    assert(fired.size() == 2 && finishedCount == 2);
}

void testDirectorFailsClosed() {
    TempProject project("saida_seq_director_fail_test");
    project.write("intro.sseq", directorSequenceJson("Ghost"));

    saida::Rig rig = makeRig();
    auto walk = makeClip("walk", 0.0f, 2.0f);

    saida::Node root("Root");
    saida::Node* hero = root.addChild(std::make_unique<saida::Node>("Hero"));
    auto* animator = hero->addBehaviour<saida::Animator>();
    animator->setRig(&rig);
    animator->addClip("walk", walk.get());

    auto* director = root.addBehaviour<saida::SequenceDirectorBehaviour>();
    director->sequence = "intro.sseq";

    std::vector<std::string> fired;
    int finishedCount = 0;
    saida::Connection onEvent = director->sequenceEvent.connect(
        [&](std::string name) { fired.push_back(std::move(name)); });
    saida::Connection onFinished =
        director->sequenceFinished.connect([&]() { ++finishedCount; });

    // Cible "Ghost" absente : réessaie pendant le délai puis échoue fermé,
    // sans jamais émettre d'événement ni de fin.
    for (int i = 0; i < 16; ++i) director->onUpdate(0.5f);
    assert(fired.empty() && finishedCount == 0);

    // Séquence introuvable : échec immédiat, silencieux côté signaux.
    auto* missing = root.addBehaviour<saida::SequenceDirectorBehaviour>();
    missing->sequence = "absent.sseq";
    saida::Connection onMissing = missing->sequenceFinished.connect([&]() { ++finishedCount; });
    for (int i = 0; i < 8; ++i) missing->onUpdate(0.5f);
    assert(finishedCount == 0);
}

} // namespace

int main() {
    saida::registerReflectedTypes();
    testRoundtripAndValidation();
    testDeterministicPlayback();
    testDirectorPlaysSceneSequence();
    testDirectorFailsClosed();
    std::puts("saida_animation_sequence_tests: OK");
    return 0;
}
