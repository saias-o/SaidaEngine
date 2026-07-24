#include "editor/InspectorRegistry.hpp"
#include "editor/EditorUI.hpp"

#include "scene/Behaviour.hpp"
#include "scene/Node.hpp"
#include "behaviours/CharacterBehaviour.hpp"
#include "behaviours/CameraFollowBehaviour.hpp"
#include "behaviours/SpawnerBehaviour.hpp"
#include "audio/AudioSourceBehaviour.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "scripting/ScriptBehaviour.hpp"
#include "scenario/ScenarioAsset.hpp"
#include "scenario/ScenarioRunnerBehaviour.hpp"

#include <imgui.h>

#include <array>
#include <cstring>
#include <string>
#include <variant>
#include <vector>

namespace saida {
namespace {

// Small helper: an ImGui text field bound to a std::string; returns true on edit.
template <size_t N>
bool textField(const char* label, std::string& value) {
    std::array<char, N> buf{};
    std::strncpy(buf.data(), value.c_str(), buf.size() - 1);
    if (ImGui::InputText(label, buf.data(), buf.size())) {
        value = buf.data();
        return true;
    }
    return false;
}

void registerBuiltins(InspectorRegistry& reg) {
    reg.registerDrawer("AudioSource", [](Behaviour& b, EditorUI& ed) {
        auto& a = static_cast<AudioSourceBehaviour&>(b);
        if (textField<128>("Audio Name", a.audioName)) ed.markDirty();
    });

    reg.registerDrawer("Spawner", [](Behaviour& b, EditorUI& ed) {
        auto& s = static_cast<SpawnerBehaviour&>(b);
        bool changed = textField<256>("Scene Path", s.scenePath);
        changed |= ImGui::DragFloat("Interval (s)", &s.interval, 0.05f, 0.0f, 60.0f);
        changed |= ImGui::DragFloat("Lifetime (s)", &s.lifetime, 0.05f, 0.0f, 60.0f);
        if (changed) ed.markDirty();
    });

    reg.registerDrawer("ScenarioRunner", [](Behaviour& b, EditorUI& ed) {
        auto& s = static_cast<ScenarioRunnerBehaviour&>(b);
        bool changed = false;
        changed |= textField<512>("Scenario", s.scenarioPath);
        changed |= ImGui::Checkbox("Auto Start", &s.autoStart);
        changed |= ImGui::Checkbox("Cleanup On Stop", &s.cleanupOnStop);
        if (changed) ed.markDirty();

        if (ImGui::Button("Validate")) {
            saida::ScenarioAsset asset;
            std::vector<saida::ScenarioIssue> issues;
            saida::ScenarioAsset::loadFromFile(s.scenarioPath, asset, &issues);
            if (issues.empty()) ImGui::OpenPopup("ScenarioValidPopup");
        }
        ImGui::SameLine();
        if (ImGui::Button("Start")) s.start();
        ImGui::SameLine();
        if (ImGui::Button("Stop")) s.stop();
        ImGui::TextDisabled("State: %s%s", s.running() ? "Running" : "Stopped", s.paused() ? " (Paused)" : "");
        if (!s.currentStep().empty()) ImGui::TextDisabled("Step: %s", s.currentStep().c_str());
        if (!s.lastError().empty()) ImGui::TextWrapped("Last error: %s", s.lastError().c_str());
        if (ImGui::BeginPopup("ScenarioValidPopup")) {
            ImGui::TextUnformatted("Scenario valid");
            ImGui::EndPopup();
        }
    });

    reg.registerDrawer("Character", [](Behaviour& b, EditorUI& ed) {
        auto& c = static_cast<CharacterBehaviour&>(b);
        bool changed = false;
        changed |= ImGui::DragFloat("Move Speed", &c.moveSpeed, 0.1f, 0.1f, 50.0f);
        changed |= ImGui::DragFloat("Sprint x", &c.sprintMultiplier, 0.05f, 1.0f, 5.0f);
        changed |= ImGui::DragFloat("Jump Force", &c.jumpForce, 0.1f, 0.1f, 50.0f);
        changed |= ImGui::DragFloat("Gravity", &c.gravity, 0.1f, 0.1f, 50.0f);
        changed |= ImGui::Checkbox("Face Movement", &c.faceMovement);
        changed |= ImGui::DragFloat("Turn Speed", &c.turnSpeed, 0.5f, 1.0f, 50.0f);
        if (CharacterBodyNode* body = c.node() ? c.node()->asCharacterBody() : nullptr)
            ImGui::TextDisabled("On floor: %s", body->isOnFloor() ? "yes" : "no");
        else
            ImGui::TextDisabled("(attach to a CharacterBody node)");

        ImGui::SeparatorText("Animation clips (child Animator)");
        changed |= textField<64>("Idle", c.idleClip);
        changed |= textField<64>("Walk", c.walkClip);
        changed |= textField<64>("Jump", c.jumpClip);
        if (changed) ed.markDirty();
    });

    reg.registerDrawer("CameraFollow", [](Behaviour& b, EditorUI& ed) {
        auto& f = static_cast<CameraFollowBehaviour&>(b);
        bool changed = false;
        changed |= textField<64>("Target Group", f.targetGroup);
        ImGui::SeparatorText("Rig");
        changed |= ImGui::DragFloat("Distance", &f.distance, 0.1f, 0.5f, 50.0f);
        changed |= ImGui::DragFloat("Height", &f.height, 0.05f, 0.0f, 10.0f);
        changed |= ImGui::DragFloat("Shoulder Offset", &f.shoulderOffset, 0.05f, -5.0f, 5.0f);
        ImGui::SeparatorText("Orbit");
        changed |= ImGui::DragFloat("Yaw Sensitivity", &f.yawSensitivity, 0.01f, 0.0f, 2.0f);
        changed |= ImGui::DragFloat("Pitch Sensitivity", &f.pitchSensitivity, 0.01f, 0.0f, 2.0f);
        changed |= ImGui::DragFloat("Min Pitch", &f.minPitch, 0.5f, -89.0f, 0.0f);
        changed |= ImGui::DragFloat("Max Pitch", &f.maxPitch, 0.5f, 0.0f, 89.0f);
        ImGui::SeparatorText("Smoothing & walls");
        changed |= ImGui::DragFloat("Position Damping", &f.positionDamping, 0.5f, 1.0f, 50.0f);
        changed |= ImGui::DragFloat("Collision Margin", &f.collisionMargin, 0.01f, 0.0f, 2.0f);
        changed |= ImGui::DragFloat("Min Distance", &f.minDistance, 0.05f, 0.1f, 10.0f);
        if (changed) ed.markDirty();
    });

    reg.registerDrawer("ScriptBehaviour", [](Behaviour& b, EditorUI& ed) {
        auto& sc = static_cast<ScriptBehaviour&>(b);

        std::string path = sc.scriptPath();
        if (textField<512>("Script", path)) { sc.setScriptPath(path); ed.markDirty(); }
        if (ImGui::Button("Reload")) sc.reload();
        ImGui::SameLine();
        ImGui::TextDisabled(sc.loaded() ? "Loaded" : "Not loaded");
        bool hot = sc.hotReloadEnabled();
        if (ImGui::Checkbox("Hot Reload", &hot)) { sc.setHotReloadEnabled(hot); ed.markDirty(); }

        if (sc.properties().empty()) return;
        if (!ImGui::CollapsingHeader("Script Properties", ImGuiTreeNodeFlags_DefaultOpen)) return;
        for (auto& property : sc.properties()) {
            ImGui::PushID(property.name.c_str());
            bool changed = false;
            switch (property.type) {
            case ScriptProperty::Type::Number: {
                double v = std::get<double>(property.value);
                if ((changed = ImGui::InputDouble(property.name.c_str(), &v))) property.value = v;
                break;
            }
            case ScriptProperty::Type::Boolean: {
                bool v = std::get<bool>(property.value);
                if ((changed = ImGui::Checkbox(property.name.c_str(), &v))) property.value = v;
                break;
            }
            case ScriptProperty::Type::String: {
                std::string v = std::get<std::string>(property.value);
                if ((changed = textField<512>(property.name.c_str(), v))) property.value = v;
                break;
            }
            }
            if (changed) { sc.applyProperty(property); ed.markDirty(); }
            ImGui::PopID();
        }
    });
}

} // namespace

InspectorRegistry& InspectorRegistry::instance() {
    static InspectorRegistry registry;
    static const bool initialized = [] { registerBuiltins(registry); return true; }();
    (void)initialized;
    return registry;
}

void InspectorRegistry::registerDrawer(std::string typeName, Drawer drawer) {
    drawers_[std::move(typeName)] = std::move(drawer);
}

bool InspectorRegistry::draw(Behaviour& b, EditorUI& editor) const {
    const char* type = b.typeName();
    if (!type) return false;
    auto it = drawers_.find(type);
    if (it == drawers_.end()) return false;
    it->second(b, editor);
    return true;
}

} // namespace saida
