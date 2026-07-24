#include "scene/animation/SequenceDirectorBehaviour.hpp"

#include "core/Log.hpp"
#include "core/Paths.hpp"
#include "scene/Node.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"

#include <filesystem>

namespace saida {

namespace {

// Imported rigs sometimes arrive a few frames after the scene mounts; beyond
// this delay, a missing target is a definitive error.
constexpr float kBindDeadlineSeconds = 5.0f;

Node* sceneRootOf(Node* n) {
    while (n && n->parent()) n = n->parent();
    return n;
}

Node* findNodeByName(Node* root, const std::string& name) {
    if (!root) return nullptr;
    if (root->name() == name) return root;
    for (const auto& child : root->children())
        if (Node* hit = findNodeByName(child.get(), name)) return hit;
    return nullptr;
}

// Same key rule as Animator::playView: the part after '#' names the clip in
// the target Animator's library.
const AnimationClip* clipFromLibrary(const Animator& animator, const std::string& key) {
    const size_t hash = key.rfind('#');
    const std::string clipName = hash == std::string::npos ? key : key.substr(hash + 1);
    auto it = animator.clips().find(clipName);
    return it != animator.clips().end() ? it->second : nullptr;
}

} // namespace

void SequenceDirectorBehaviour::onUpdate(float dt) {
    if (bindState_ == BindState::Failed) return;
    if (bindState_ == BindState::Unbound && !tryBind(dt)) return;

    if (autoplay && !autoplayConsumed_) {
        autoplayConsumed_ = true;
        playRequested_ = true;
    }
    if (playRequested_) {
        playRequested_ = false;
        player_.seek(0.0f);
        playing_ = true;
        finishedEmitted_ = false;
    }
    if (!playing_) return;

    player_.update(dt);
    if (player_.finished()) {
        playing_ = false;
        if (!finishedEmitted_) {
            finishedEmitted_ = true;
            sequenceFinished.emit();
        }
    }
}

void SequenceDirectorBehaviour::play() { playRequested_ = true; }

void SequenceDirectorBehaviour::stop() {
    playing_ = false;
    playRequested_ = false;
}

bool SequenceDirectorBehaviour::tryBind(float dt) {
    if (sequence.empty()) {
        failWith({}, "config (empty 'sequence' path)");
        return false;
    }

    std::string path = sequence;
    if (std::filesystem::path p(sequence); p.is_relative() && !activeProjectRoot().empty())
        path = (std::filesystem::path(activeProjectRoot()) / p).string();

    AnimationSequenceParseResult parsed = AnimationSequence::loadFile(path);
    std::vector<AssetDiagnostic> diags = parsed.diagnostics;
    if (parsed.ok) {
        std::vector<AssetDiagnostic> more = parsed.sequence.validate();
        diags.insert(diags.end(), more.begin(), more.end());
    }
    if (!parsed.ok || hasErrors(diags)) {
        failWith(diags, "parse/validate");
        return false;
    }

    Node* root = sceneRootOf(node());

    // bind() resolves a track's animator THEN its clips, sequentially:
    // `current` therefore always refers to the current track's Animator.
    Animator* current = nullptr;
    const auto resolveAnimator = [&](const std::string& target) -> Animator* {
        Node* hit = findNodeByName(root, target);
        Animator* animator = hit ? hit->getBehaviour<Animator>() : nullptr;
        if (hit && !animator) animator = hit->findBehaviourInChildren<Animator>();
        current = animator;
        return animator;
    };
    const auto resolveClip = [&](const std::string& key) -> const AnimationClip* {
        return current ? clipFromLibrary(*current, key) : nullptr;
    };
    const auto bindProperty = [&](const std::string& target,
                                  TimelinePropertyTrack& track) -> bool {
        const size_t dot = target.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= target.size()) return false;
        Node* hit = findNodeByName(root, target.substr(0, dot));
        if (!hit) return false;
        const std::string property = target.substr(dot + 1);
        auto& registry = reflect::TypeRegistry::instance();
        if (const reflect::TypeDesc* desc = registry.find(hit->typeName()))
            if (desc->findProperty(property)) {
                track.bind(hit, *desc, property);
                return true;
            }
        for (const auto& behaviour : hit->behaviours()) {
            if (!behaviour->typeName()) continue;
            const reflect::TypeDesc* desc = registry.find(behaviour->typeName());
            if (desc && desc->findProperty(property)) {
                track.bind(behaviour.get(), *desc, property);
                return true;
            }
        }
        return false;
    };

    std::vector<AssetDiagnostic> bindDiags;
    const bool bound = player_.bind(parsed.sequence, resolveAnimator, resolveClip,
                                    bindProperty, &bindDiags);
    if (!bound || hasErrors(bindDiags)) {
        bindWait_ += dt;
        if (bindWait_ < kBindDeadlineSeconds) return false;  // target might still be on the way
        failWith(bindDiags, "bind");
        return false;
    }

    listen(player_.sequenceEvent,
           [this](const std::string& name) { sequenceEvent.emit(name); });
    bindState_ = BindState::Bound;
    return true;
}

void SequenceDirectorBehaviour::failWith(const std::vector<AssetDiagnostic>& diags,
                                         const char* stage) {
    bindState_ = BindState::Failed;
    Log::warn("SequenceDirector: sequence '", sequence, "' on node '",
              node() ? node()->name() : "?", "' rejected at ", stage);
    for (const AssetDiagnostic& d : diags)
        Log::warn("  [", d.code, "] ", d.jsonPath, " ", d.message);
}

void SequenceDirectorBehaviour::describe(reflect::TypeBuilder<SequenceDirectorBehaviour>& t) {
    t.doc("Plays a .sseq multi-track sequence in the running scene. Animation "
          "tracks drive the Animator of the node named by the track target, "
          "property tracks drive a reflected property addressed as "
          "'Node.property', and the event track is re-emitted through the "
          "'sequenceEvent' signal. Invalid sequences or missing targets fail "
          "closed with logged diagnostics.");
    t.property("sequence", &SequenceDirectorBehaviour::sequence).asset()
        .tooltip(".sseq path (project-relative)");
    t.property("autoplay", &SequenceDirectorBehaviour::autoplay)
        .tooltip("start playback when the scene starts");
    t.signal("sequenceEvent", &SequenceDirectorBehaviour::sequenceEvent);
    t.signal("sequenceFinished", &SequenceDirectorBehaviour::sequenceFinished);
    t.slot("play", &SequenceDirectorBehaviour::play);
    t.slot("stop", &SequenceDirectorBehaviour::stop);
}

} // namespace saida
