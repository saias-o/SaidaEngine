// saida_animation_bench — headless benchmark of the animation runtime. Builds a
// synthetic humanoid-sized rig and clip, then measures the same population on
// the two playback paths: the authoring graph (Animator::onUpdate) and the
// data-oriented kernel (cooked clip + AnimationInstance). No GPU, no window:
// the numbers are the CPU cost the renderer would pay before palette upload.
//
// Usage: saida_animation_bench [--chars N] [--bones N] [--keys N] [--frames N]

#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/AnimationCooker.hpp"
#include "scene/animation/AnimationProgram.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/Rig.hpp"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

int argInt(int argc, char** argv, const char* name, int fallback) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
    return fallback;
}

// A chain-heavy rig: a root plus branches of ~8 bones, like limbs off a spine.
saida::Rig makeRig(int boneCount) {
    saida::Rig rig;
    for (int i = 0; i < boneCount; ++i) {
        const int parent = i == 0 ? -1 : (i % 8 == 0 ? 0 : i - 1);
        saida::Transform rest;
        rest.position = {0.0f, 0.25f, 0.0f};
        rig.addBone("bone_" + std::to_string(i), parent, glm::mat4(1.0f), rest);
    }
    std::string error;
    if (!rig.finalize(&error)) {
        std::fprintf(stderr, "invalid bench rig: %s\n", error.c_str());
        std::exit(1);
    }
    return rig;
}

// Every bone gets a rotation + translation track with `keyCount` linear keys.
std::unique_ptr<saida::AnimationClip> makeClip(int boneCount, int keyCount, float duration) {
    auto clip = std::make_unique<saida::AnimationClip>("bench", duration);
    for (int b = 0; b < boneCount; ++b) {
        const std::string name = "bone_" + std::to_string(b);

        auto rot = std::make_unique<saida::TypedAnimTrack<glm::quat>>();
        rot->target = saida::TrackTarget::Rotation;
        rot->timestamps.resize(keyCount);
        rot->values.resize(keyCount);
        auto pos = std::make_unique<saida::TypedAnimTrack<glm::vec3>>();
        pos->target = saida::TrackTarget::Translation;
        pos->timestamps.resize(keyCount);
        pos->values.resize(keyCount);

        for (int k = 0; k < keyCount; ++k) {
            const float t = duration * float(k) / float(keyCount - 1);
            const float phase = t * 3.0f + float(b) * 0.1f;
            rot->timestamps[k] = t;
            rot->values[k] = glm::angleAxis(0.5f * std::sin(phase), glm::vec3(0, 0, 1));
            pos->timestamps[k] = t;
            pos->values[k] = {0.0f, 0.25f + 0.02f * std::sin(phase), 0.0f};
        }
        clip->addTrack(name, std::move(rot));
        clip->addTrack(name, std::move(pos));
    }
    return clip;
}

struct BenchStats {
    double avg, p50, p95;
};

template <typename Step>
BenchStats measure(int frames, const Step& step) {
    std::vector<double> frameMs(static_cast<size_t>(frames));
    for (int f = 0; f < frames; ++f) {
        const auto t0 = std::chrono::steady_clock::now();
        step();
        const auto t1 = std::chrono::steady_clock::now();
        frameMs[size_t(f)] = std::chrono::duration<double, std::milli>(t1 - t0).count();
    }
    std::vector<double> sorted = frameMs;
    std::sort(sorted.begin(), sorted.end());
    double total = 0.0;
    for (double ms : frameMs) total += ms;
    return {total / double(frames), sorted[sorted.size() / 2],
            sorted[size_t(double(sorted.size()) * 0.95)]};
}

void printStats(const char* label, const BenchStats& stats, int chars, int bones) {
    std::printf("  %-16s avg %.3f ms | p50 %.3f ms | p95 %.3f ms | %.1f us/char | %.2f M bone poses/s\n",
                label, stats.avg, stats.p50, stats.p95, stats.avg * 1000.0 / double(chars),
                double(chars) * double(bones) * (1000.0 / stats.avg) / 1e6);
}

} // namespace

bool hasFlag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

int main(int argc, char** argv) {
    const int chars  = argInt(argc, argv, "--chars", 100);
    const int bones  = argInt(argc, argv, "--bones", 80);
    const int keys   = argInt(argc, argv, "--keys", 60);
    const int frames = argInt(argc, argv, "--frames", 300);
    const bool breakdown = hasFlag(argc, argv, "--breakdown");
    const int warmup = 10;
    const float dt = 1.0f / 60.0f;

    saida::Rig rig = makeRig(bones);
    auto clip = makeClip(bones, keys, 2.0f);

    // Chemin d'authoring : Animator + graphe d'AnimNode.
    std::vector<std::unique_ptr<saida::Animator>> animators;
    animators.reserve(size_t(chars));
    for (int i = 0; i < chars; ++i) {
        auto anim = std::make_unique<saida::Animator>();
        anim->setRig(&rig);
        anim->addClip("bench", clip.get());
        anim->play("bench", true, 0.0f);
        anim->onUpdate(dt * float(i % 17));  // désynchronise la population
        animators.push_back(std::move(anim));
    }

    // Chemin runtime : clip cuit + programme compilé + instances sans allocation.
    saida::CookReport report;
    auto cooked = std::make_shared<saida::CookedClip>(
        saida::AnimationCooker::cook(*clip, rig, saida::CookSettings{}, &report));
    auto program = saida::AnimationProgram::singleClip(cooked);
    std::vector<saida::AnimationInstance> instances;
    instances.reserve(size_t(chars));
    for (int i = 0; i < chars; ++i) {
        instances.emplace_back(program, rig);
        instances.back().update(dt * float(i % 17));
    }

    for (int f = 0; f < warmup; ++f) {
        for (auto& a : animators) a->onUpdate(dt);
        for (auto& i : instances) i.update(dt);
    }

    const BenchStats authoring =
        measure(frames, [&] { for (auto& a : animators) a->onUpdate(dt); });
    const BenchStats cookedPath =
        measure(frames, [&] { for (auto& i : instances) i.update(dt); });

    std::printf("saida_animation_bench: %d chars x %d bones, %d keys/track, %d frames\n",
                chars, bones, keys, frames);
    printStats("authoring graph:", authoring, chars, bones);
    printStats("cooked kernel:", cookedPath, chars, bones);
    std::printf("  cooked clip: %u -> %u keys, %zu -> %zu bytes (%.1f%%)\n",
                report.sourceKeys, report.cookedKeys, report.sourceBytes,
                report.cookedBytes,
                report.sourceBytes > 0
                    ? 100.0 * double(report.cookedBytes) / double(report.sourceBytes)
                    : 0.0);

    // Décomposition par phase du chemin cuit : sampling seul (curseurs) puis
    // solve seul (blend + matrices globales), sur la même population.
    if (breakdown) {
        std::vector<saida::CookedCursor> cursors(cooked->trackCount());
        saida::LocalPose pose;
        pose.resize(size_t(bones));
        for (int b = 0; b < bones; ++b)
            pose.localTransforms[size_t(b)] = rig.bones()[size_t(b)].restLocal;
        float t = 0.0f;
        const BenchStats sampling = measure(frames, [&] {
            for (int c = 0; c < chars; ++c) cooked->sample(t, pose, cursors.data());
            t += dt;
            if (t > cooked->duration()) t = 0.0f;
        });
        saida::GlobalPose global;
        global.resize(size_t(bones));
        const BenchStats solve = measure(frames, [&] {
            for (int c = 0; c < chars; ++c) global.computeFrom(pose, rig);
        });
        printStats("  sampling only:", sampling, chars, bones);
        printStats("  solve only:", solve, chars, bones);
    }

    // Sanity : la pose a réellement bougé sur les deux chemins (garde contre une
    // boucle optimisée à vide ou une FSM restée sur place).
    if (animators[0]->globalPose().globalMatrices.size() != size_t(bones) ||
        instances[0].globalPose().globalMatrices.size() != size_t(bones)) {
        std::fprintf(stderr, "unexpected pose size\n");
        return 1;
    }
    return 0;
}
