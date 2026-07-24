#include "editor/panels/ModelImporterPanel.hpp"

#include "cli/MeshoptGlbExporter.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/LightNode.hpp"
#include "project/Project.hpp"
#include "scene/GLTFLoader.hpp"
#include "scene/Scene.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/AnimStateMachine.hpp"
#include "scene/animation/ClipNode.hpp"
#include "scene/animation/AnimationClip.hpp"

#include <filesystem>
#include <imgui.h>

#include <algorithm>
#include <string>
#include <vector>

namespace saida {

ModelImporterPanel::~ModelImporterPanel() = default;

void ModelImporterPanel::open(const std::string& path, Project* project,
                              ResourceManager* resources) {
    active_ = true;
    modelPath_ = path;
    previewScene_ = std::make_unique<Scene>();

    auto light = previewScene_->createChild<LightNode>("PreviewLight");
    light->transform().position = glm::vec3(2.0f, 2.0f, 2.0f);
    light->direction =
        glm::normalize(glm::vec3(0.0f) - light->transform().position);

    if (resources) {
        GLTFLoadOptions options;
        if (project) options.autoMeshLods = project->autoMeshLods();
        GLTFLoader::load(path, *previewScene_, *resources, options);
    }
}

void ModelImporterPanel::close() {
    active_ = false;
    previewScene_.reset();
}

void ModelImporterPanel::draw() {
    if (!active_) return;

    // The panel used to be recreated every frame; preserve the result message's
    // one-frame lifetime while moving the actual preview lifetime here.
    exportStatus_.clear();
    bool open = active_;
    bool visible = ImGui::Begin("3D Importer", &open);
    bool closeRequested = !open;
    if (!visible) {
        ImGui::End();
        if (closeRequested) close();
        return;
    }

    ImGui::Text("Previewing Model:");
    ImGui::TextDisabled("%s", modelPath_.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    drawAnimationControls(findAnimator());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    drawExport();

    ImGui::Spacing();
    if (ImGui::Button("Close Preview", ImVec2(150, 30))) {
        closeRequested = true;
    }

    ImGui::End();
    if (closeRequested) close();
}

Animator* ModelImporterPanel::findAnimator() const {
    Animator* animator = nullptr;
    if (previewScene_) {
        previewScene_->traverse([&](Node& node, const glm::mat4&) {
            if (!animator && node.getBehaviour<Animator>()) {
                animator = node.getBehaviour<Animator>();
            }
        });
    }
    return animator;
}

void ModelImporterPanel::drawAnimationControls(Animator* animator) {
    if (animator && !animator->clips().empty()) {
        ImGui::Text("Animation Controls");
        ImGui::Spacing();

        // Clip selector — stable order for a deterministic combo.
        std::vector<std::string> clipNames;
        clipNames.reserve(animator->clips().size());
        for (const auto& [name, clip] : animator->clips())
            clipNames.push_back(name);
        std::sort(clipNames.begin(), clipNames.end());

        const std::string& current = animator->currentClip();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##Clip", current.empty() ? "(select a clip)" : current.c_str())) {
            for (const auto& name : clipNames) {
                if (ImGui::Selectable(name.c_str(), name == current)) {
                    animator->play(name);
                    if (ClipNode* c = animator->activeClipNode())
                        c->setPlaybackSpeed(playbackSpeed_);
                }
            }
            ImGui::EndCombo();
        }

        if (ClipNode* clipNode = animator->activeClipNode()) {
            bool isPlaying = playbackSpeed_ > 0.0f;

            if (isPlaying) {
                if (ImGui::Button("Pause", ImVec2(80, 0))) {
                    playbackSpeed_ = 0.0f;
                    clipNode->setPlaybackSpeed(playbackSpeed_);
                }
            } else {
                if (ImGui::Button("Play", ImVec2(80, 0))) {
                    playbackSpeed_ = 1.0f;
                    clipNode->setPlaybackSpeed(playbackSpeed_);
                }
            }

            ImGui::SameLine();
            float currentTime = clipNode->time();
            float dur = clipNode->duration();
            if (dur == 0.0f) dur = 1.0f; // prevent division by zero in slider

            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##Time", &currentTime, 0.0f, dur, "Time: %.2fs")) {
                clipNode->setTime(currentTime);
            }

            ImGui::Spacing();
        } else if (animator->rootNode()) {
            ImGui::TextDisabled("Animation uses a complex graph (not a simple clip).");
        }
    } else if (animator && animator->rootNode()) {
        ImGui::TextDisabled("Animation uses a complex graph (not a simple clip).");
    } else {
        ImGui::TextDisabled("No animation detected in this model.");
    }
}

void ModelImporterPanel::drawExport() {
    // Export meshopt GLB: rewrites the previewed model's geometry to
    // .meshopt.glb (quantized buffers + EXT_meshopt_compression) next to the
    // source — the compact format intended for web packages.
    if (ImGui::Button("Export meshopt GLB", ImVec2(180, 30))) {
        std::vector<ExportMesh> meshes;
        std::string error;
        const std::string outPath =
            std::filesystem::path(modelPath_)
                .replace_extension(".meshopt.glb").string();
        if (!collectExportMeshes(modelPath_, meshes, error)) {
            exportStatus_ = "Export failed: " + error;
        } else if (exportMeshoptGlb(meshes, outPath)) {
            exportStatus_ = "Exported " + std::to_string(meshes.size()) + " mesh(es) to " + outPath;
        } else {
            exportStatus_ = "Export failed (see log)";
        }
    }
    if (!exportStatus_.empty()) ImGui::TextWrapped("%s", exportStatus_.c_str());
}

} // namespace saida
