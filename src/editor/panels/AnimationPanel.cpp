#include "editor/panels/AnimationPanel.hpp"

#include "graphics/ResourceManager.hpp"
#include "project/AssetRegistry.hpp"
#include "project/Project.hpp"
#include "scene/Scene.hpp"
#include "scene/animation/AnimGraphAsset.hpp"
#include "scene/animation/AnimStateMachine.hpp"
#include "scene/animation/AnimationClip.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/ClipView.hpp"
#include "scene/animation/RigAsset.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace saida {

namespace {

struct FoundAnimator {
    Animator* animator = nullptr;
    std::string nodeName;
};

// The driven Animator: the selected node's first, otherwise the first one
// in the preview scene (importer mode), otherwise the first one in the scene.
FoundAnimator findAnimator(EditorUI* editor, Scene* scene) {
    FoundAnimator found;
    if (Node* selected = editor->selectedNode()) {
        if (auto* anim = selected->getBehaviour<Animator>()) {
            return {anim, selected->name()};
        }
    }
    Scene* scenes[2] = {editor->previewScene(), scene};
    for (Scene* candidate : scenes) {
        if (!candidate || found.animator) continue;
        candidate->traverse([&](Node& node, const glm::mat4&) {
            if (!found.animator) {
                if (auto* anim = node.getBehaviour<Animator>()) {
                    found.animator = anim;
                    found.nodeName = node.name();
                }
            }
        });
    }
    return found;
}

std::vector<std::string> sortedClipNames(const Animator& animator) {
    std::vector<std::string> names;
    names.reserve(animator.clips().size());
    for (const auto& [name, clip] : animator.clips()) names.push_back(name);
    std::sort(names.begin(), names.end());
    return names;
}

// Sub-asset key for a library clip ("model.glb#Run"), looked up via the
// registry. Falls back to "#name" if the clip isn't registered: playView
// resolves by the name after '#', so the view stays playable locally.
std::string subAssetKeyFor(const AnimationClip* clip, const std::string& clipName,
                           ResourceManager* resources) {
    if (resources && resources->registry()) {
        const AssetID id = resources->animationId(clip);
        if (id != kAssetInvalid) {
            const std::string key = resources->registry()->getPath(id);
            if (!key.empty()) return key;
        }
    }
    return "#" + clipName;
}

// An asset's diagnostics, shown in yellow (warning) or red (error).
void drawDiagnostics(const std::vector<AssetDiagnostic>& diagnostics) {
    for (const auto& d : diagnostics) {
        const bool isError = d.severity == AssetDiagnostic::Severity::Error;
        ImGui::PushStyleColor(ImGuiCol_Text, isError ? ImVec4(1.0f, 0.4f, 0.4f, 1.0f)
                                                     : ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
        ImGui::TextWrapped("%s %s", isError ? "[error]" : "[warn]", d.message.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace

// Cached scan of the project's animation assets, project-relative paths.
void AnimationPanel::refreshAssetList(EditorUI* editor, Project* project) {
    const double now = ImGui::GetTime();
    if (editor->animAssetScanTime_ >= 0.0 && now - editor->animAssetScanTime_ < 2.0) return;
    editor->animAssetScanTime_ = now;
    editor->animAssetFiles_.clear();
    if (!project || !project->isLoaded()) return;

    const std::filesystem::path root(project->rootPath());
    std::error_code ec;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto& entry = *it;
        if (entry.is_directory(ec)) {
            const std::string dir = entry.path().filename().string();
            if (dir == "build" || dir == ".git" || dir == ".saida") it.disable_recursion_pending();
            continue;
        }
        const std::string ext = entry.path().extension().string();
        if (ext == ".srig" || ext == ".sclip" || ext == ".sgraph" || ext == ".sseq") {
            editor->animAssetFiles_.push_back(
                entry.path().lexically_relative(root).generic_string());
        }
    }
    std::sort(editor->animAssetFiles_.begin(), editor->animAssetFiles_.end());
}

static void drawPlaybackSection(Animator* animator) {
    if (!ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) return;

    const std::vector<std::string> clipNames = sortedClipNames(*animator);
    const std::string& current = animator->currentClip();

    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##PlayClip", current.empty() ? "(clip)" : current.c_str())) {
        for (const auto& name : clipNames) {
            if (ImGui::Selectable(name.c_str(), name == current)) animator->play(name);
        }
        ImGui::EndCombo();
    }

    ClipNode* node = animator->activeClipNode();
    if (!node) {
        if (animator->rootNode())
            ImGui::TextDisabled("The active graph isn't a simple clip.");
        return;
    }

    static float speed = 1.0f;
    const bool playing = speed != 0.0f;
    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(80, 0))) {
        speed = playing ? 0.0f : 1.0f;
        node->setPlaybackSpeed(speed);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    if (ImGui::DragFloat("##Speed", &speed, 0.01f, -4.0f, 4.0f, "x%.2f"))
        node->setPlaybackSpeed(speed);
    ImGui::SameLine();

    // Scrubbing happens within the playable window (including a view's range).
    float time = node->time();
    const float start = node->rangeStart();
    const float end = node->rangeEnd() > start ? node->rangeEnd() : start + 1.0f;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##Time", &time, start, end, "%.2fs")) node->setTime(time);
}

void AnimationPanel::drawClipViewSection(EditorUI* editor, Animator* animator, ResourceManager* resources,
                         Project* project) {
    if (!ImGui::CollapsingHeader("Clip View (.sclip)", ImGuiTreeNodeFlags_DefaultOpen)) return;

    const std::vector<std::string> clipNames = sortedClipNames(*animator);
    if (clipNames.empty()) {
        ImGui::TextDisabled("No clips in this Animator.");
        return;
    }

    ImGui::InputText("Name", editor->animViewName_, sizeof(editor->animViewName_));

    editor->animViewSourceIndex_ =
        std::clamp(editor->animViewSourceIndex_, 0, int(clipNames.size()) - 1);
    const std::string& sourceName = clipNames[size_t(editor->animViewSourceIndex_)];
    if (ImGui::BeginCombo("Source", sourceName.c_str())) {
        for (int i = 0; i < int(clipNames.size()); ++i) {
            if (ImGui::Selectable(clipNames[size_t(i)].c_str(),
                                  i == editor->animViewSourceIndex_)) {
                editor->animViewSourceIndex_ = i;
                editor->animViewEnd_ = 0.0f;  // re-frame the range for the new clip
            }
        }
        ImGui::EndCombo();
    }

    const AnimationClip* sourceClip = animator->clips().at(sourceName);
    const float duration = sourceClip->duration();
    if (editor->animViewEnd_ <= 0.0f) editor->animViewEnd_ = duration;
    editor->animViewStart_ = std::clamp(editor->animViewStart_, 0.0f, duration);
    editor->animViewEnd_ = std::clamp(editor->animViewEnd_, 0.0f, duration);

    ImGui::DragFloatRange2("Range", &editor->animViewStart_, &editor->animViewEnd_, 0.01f,
                           0.0f, duration, "start %.2fs", "end %.2fs");
    ImGui::Checkbox("Loop", &editor->animViewLoop_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90);
    ImGui::DragFloat("Speed", &editor->animViewSpeed_, 0.01f, -4.0f, 4.0f, "x%.2f");

    // The view is built field by field then validated against the source: the
    // diagnostics show live, and saving is blocked on error.
    ClipView view;
    view.name = editor->animViewName_;
    view.source = subAssetKeyFor(sourceClip, sourceName, resources);
    view.hasRange = editor->animViewStart_ > 0.0f || editor->animViewEnd_ < duration;
    view.rangeStart = editor->animViewStart_;
    view.rangeEnd = editor->animViewEnd_;
    view.loop = editor->animViewLoop_;
    view.speed = editor->animViewSpeed_;

    std::vector<AssetDiagnostic> diags = view.validate(sourceClip);
    if (view.name.empty())
        diags.push_back({"clipview.name.missing", AssetDiagnostic::Severity::Error, "/name",
                         "the view name is empty"});
    drawDiagnostics(diags);

    const bool valid = !hasErrors(diags);
    if (ImGui::Button("Play view", ImVec2(120, 0)) && valid) {
        animator->playView(view, 0.0f);
        editor->animStatus_ = "View '" + view.name + "' played.";
    }
    ImGui::SameLine();
    const bool canSave = valid && project && project->isLoaded();
    if (!canSave) ImGui::BeginDisabled();
    if (ImGui::Button("Save .sclip", ImVec2(150, 0))) {
        const std::filesystem::path dir =
            std::filesystem::path(project->rootPath()) / "assets" / "animation";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::string path = (dir / (view.name + ".sclip")).generic_string();
        if (view.saveFile(path)) {
            resources->loadClipView(path);
            editor->animStatus_ = "Saved: assets/animation/" + view.name + ".sclip";
            editor->animAssetScanTime_ = -1.0;  // force a rescan of the list
        } else {
            editor->animStatus_ = "Failed to write " + path;
        }
    }
    if (!canSave) ImGui::EndDisabled();
    if (!project || !project->isLoaded()) {
        ImGui::SameLine();
        ImGui::TextDisabled("(project required to save)");
    }
}

void AnimationPanel::completePendingAssetLoad(EditorUI* editor, Animator* animator,
                                              ResourceManager* resources) {
    using Action = EditorUI::AnimPendingAction;
    const Action action = editor->animPendingAction_;
    if (action == Action::None) return;

    const AssetID id = editor->animPendingAssetId_;
    AssetLoadState state = AssetLoadState::Failed;
    std::string error;
    if (action == Action::InspectRig) {
        state = resources->rigAssetLoadState(id);
        error = resources->rigAssetLoadError(id);
    } else if (action == Action::PlayClipView || action == Action::EditClipView) {
        state = resources->clipViewLoadState(id);
        error = resources->clipViewLoadError(id);
    } else if (action == Action::ApplyGraph) {
        state = resources->animGraphLoadState(id);
        error = resources->animGraphLoadError(id);
    }

    if (state == AssetLoadState::Queued || state == AssetLoadState::Loading) return;

    const std::string path = editor->animPendingAssetPath_;
    if (state == AssetLoadState::Failed) {
        editor->animStatus_ = "Failed to load " + path +
                              (error.empty() ? "" : ": " + error);
    } else if (action == Action::InspectRig) {
        const RigAsset* rig = resources->getRigAsset(id);
        if (rig) {
            editor->animStatus_ = "Rig '" + (rig->name.empty() ? path : rig->name) +
                                  "' loaded: " +
                                  std::to_string(rig->semantics.size()) + " semantics.";
        }
    } else if (action == Action::PlayClipView) {
        const ClipView* view = resources->getClipView(id);
        if (view) {
            animator->playView(*view, 0.0f);
            editor->animStatus_ = "View '" + view->name + "' played from " + path;
        }
    } else if (action == Action::EditClipView) {
        const ClipView* view = resources->getClipView(id);
        if (view) {
            std::snprintf(editor->animViewName_, sizeof(editor->animViewName_), "%s",
                          view->name.c_str());
            editor->animViewStart_ = view->effectiveStart();
            editor->animViewEnd_ = view->hasRange ? view->rangeEnd : 0.0f;
            editor->animViewLoop_ = view->loop;
            editor->animViewSpeed_ = view->speed;
            editor->animStatus_ = "View '" + view->name + "' loaded into the editor.";
        }
    } else if (action == Action::ApplyGraph) {
        const AnimGraphAsset* graph = resources->getAnimGraph(id);
        std::vector<AssetDiagnostic> diagnostics;
        if (graph && animator->setGraph(*graph, &diagnostics)) {
            editor->animAppliedGraphId_ = id;
            editor->animStatus_ =
                "Graph '" + (graph->name.empty() ? path : graph->name) + "' applied.";
        } else {
            editor->animStatus_ = "Failed to apply " + path +
                (diagnostics.empty() ? "" : ": " + diagnostics.front().message);
        }
    }

    editor->animPendingAction_ = Action::None;
    editor->animPendingAssetId_ = kAssetInvalid;
    editor->animPendingAssetPath_.clear();
}

void AnimationPanel::drawAssetsSection(EditorUI* editor, Animator* animator, ResourceManager* resources,
                       Project* project) {
    if (!ImGui::CollapsingHeader("Project assets", ImGuiTreeNodeFlags_DefaultOpen)) return;
    if (!project || !project->isLoaded()) {
        ImGui::TextDisabled("No project open.");
        return;
    }

    refreshAssetList(editor, project);
    if (ImGui::SmallButton("Rescan")) editor->animAssetScanTime_ = -1.0;

    if (editor->animAssetFiles_.empty()) {
        ImGui::TextDisabled("No .srig/.sclip/.sgraph/.sseq in the project.");
        return;
    }

    // Local resolution of a "file#clip" key in the selected Animator's
    // library — same rule as Animator::playView.
    const auto resolveClip = [animator](const std::string& key) -> const AnimationClip* {
        const size_t hash = key.rfind('#');
        const std::string clipName = hash == std::string::npos ? key : key.substr(hash + 1);
        auto it = animator->clips().find(clipName);
        return it != animator->clips().end() ? it->second : nullptr;
    };

    for (const std::string& rel : editor->animAssetFiles_) {
        ImGui::PushID(rel.c_str());
        ImGui::TextUnformatted(rel.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130);

        const std::string abs =
            (std::filesystem::path(project->rootPath()) / rel).generic_string();
        const bool isClipView = rel.size() > 6 && rel.rfind(".sclip") == rel.size() - 6;
        const bool isRig = rel.size() > 5 && rel.rfind(".srig") == rel.size() - 5;
        const bool isSequence = rel.size() > 5 && rel.rfind(".sseq") == rel.size() - 5;

        if (isSequence) {
            if (ImGui::SmallButton("Play")) {
                auto parsed = AnimationSequence::loadFile(abs);
                std::vector<AssetDiagnostic> diags = parsed.diagnostics;
                if (parsed.ok) {
                    auto more = parsed.sequence.validate();
                    diags.insert(diags.end(), more.begin(), more.end());
                }
                auto player = std::make_unique<SequencePlayer>();
                // In preview, every animation track is bound to the selected
                // Animator, regardless of its target name.
                if (parsed.ok && !hasErrors(diags) &&
                    player->bind(parsed.sequence, [animator](const std::string&) {
                        return animator;
                    }, resolveClip, nullptr, &diags)) {
                    editor->animSequencePlayer_ = std::move(player);
                    editor->animSequencePath_ = rel;
                    editor->animStatus_ = "Sequence '" + rel + "' bound (scrub below).";
                } else {
                    editor->animSequencePlayer_.reset();
                    editor->animStatus_ = "Invalid sequence: " + rel +
                        (diags.empty() ? "" : " — " + diags.front().message);
                }
            }
        } else if (isClipView) {
            if (ImGui::SmallButton("Play")) {
                const AssetID id = resources->loadClipView(abs);
                editor->animPendingAction_ = EditorUI::AnimPendingAction::PlayClipView;
                editor->animPendingAssetId_ = id;
                editor->animPendingAssetPath_ = rel;
                editor->animStatus_ = "Loading " + rel + "...";
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Edit")) {
                const AssetID id = resources->loadClipView(abs);
                editor->animPendingAction_ = EditorUI::AnimPendingAction::EditClipView;
                editor->animPendingAssetId_ = id;
                editor->animPendingAssetPath_ = rel;
                editor->animStatus_ = "Loading " + rel + "...";
            }
        } else if (isRig) {
            if (ImGui::SmallButton("Inspect")) {
                const AssetID id = resources->loadRigAsset(abs);
                editor->animPendingAction_ = EditorUI::AnimPendingAction::InspectRig;
                editor->animPendingAssetId_ = id;
                editor->animPendingAssetPath_ = rel;
                editor->animStatus_ = "Loading " + rel + "...";
            }
        } else {
            if (ImGui::SmallButton("Apply")) {
                const AssetID id = resources->loadAnimGraph(abs);
                editor->animPendingAction_ = EditorUI::AnimPendingAction::ApplyGraph;
                editor->animPendingAssetId_ = id;
                editor->animPendingAssetPath_ = rel;
                editor->animStatus_ = "Loading " + rel + "...";
            }
        }
        ImGui::PopID();
    }
}

void AnimationPanel::drawGraphSection(EditorUI* editor, Animator* animator, ResourceManager* resources) {
    const AnimGraphAsset* graph = resources->getAnimGraph(editor->animAppliedGraphId_);
    auto* sm = dynamic_cast<AnimStateMachine*>(animator->rootNode());
    if (!graph || !sm) return;
    if (!ImGui::CollapsingHeader("Active graph", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::Text("Graph: %s", graph->name.empty() ? "(unnamed)" : graph->name.c_str());
    ImGui::Text("State: %s",
                sm->currentState() ? sm->currentState()->name().c_str() : "(none)");
    ImGui::Separator();

    // The graph's parameters drive the blackboard live.
    for (const auto& p : graph->parameters) {
        ImGui::PushID(p.name.c_str());
        if (p.type == AnimParamType::Bool) {
            bool value = animator->blackboard().getBool(p.name);
            if (ImGui::Checkbox(p.name.c_str(), &value)) animator->setBool(p.name, value);
        } else {
            float value = animator->blackboard().getFloat(p.name);
            if (ImGui::DragFloat(p.name.c_str(), &value, 0.01f)) {
                if (p.type == AnimParamType::Int) value = float(int(value));
                animator->setFloat(p.name, value);
            }
        }
        ImGui::PopID();
    }
}

// Scrubbing the sequence in preview. Seeking is deterministic and doesn't
// emit events; playback advances the player (events are emitted).
void AnimationPanel::drawSequenceSection(EditorUI* editor) {
    SequencePlayer* player = editor->animSequencePlayer_.get();
    if (!player) return;
    if (!ImGui::CollapsingHeader("Sequence", ImGuiTreeNodeFlags_DefaultOpen)) return;

    ImGui::Text("Sequence: %s", editor->animSequencePath_.c_str());

    static bool playing = false;
    if (ImGui::Button(playing ? "Pause" : "Play", ImVec2(80, 0))) playing = !playing;
    ImGui::SameLine();
    if (ImGui::Button("Stop", ImVec2(60, 0))) {
        playing = false;
        editor->animSequencePlayer_.reset();
        return;
    }
    ImGui::SameLine();

    float time = player->time();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##SeqTime", &time, 0.0f, player->duration(), "%.2fs")) {
        playing = false;
        player->seek(time);
    } else if (playing) {
        player->update(ImGui::GetIO().DeltaTime);
        if (player->finished()) playing = false;
    }
}

void AnimationPanel::draw(EditorUI* editor, Scene* scene, ResourceManager* resources,
                          Project* project) {
    if (!editor->showAnimation_) return;

    bool open = editor->showAnimation_;
    if (!ImGui::Begin("Animation", &open)) {
        ImGui::End();
        editor->showAnimation_ = open;
        return;
    }

    FoundAnimator found = findAnimator(editor, scene);
    if (!found.animator || !found.animator->rig()) {
        ImGui::TextDisabled("No Animator in the scene.");
        ImGui::TextDisabled("Select an animated node or import a glTF model.");
        ImGui::End();
        editor->showAnimation_ = open;
        return;
    }

    ImGui::Text("Animator: %s", found.nodeName.c_str());
    ImGui::Separator();

    completePendingAssetLoad(editor, found.animator, resources);
    drawPlaybackSection(found.animator);
    drawClipViewSection(editor, found.animator, resources, project);
    drawAssetsSection(editor, found.animator, resources, project);
    drawGraphSection(editor, found.animator, resources);
    drawSequenceSection(editor);

    if (!editor->animStatus_.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", editor->animStatus_.c_str());
    }

    ImGui::End();
    editor->showAnimation_ = open;
}

} // namespace saida
