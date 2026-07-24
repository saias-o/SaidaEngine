#pragma once

#include "editor/Command.hpp"
#include "editor/EditorUI.hpp"
#include "scene/Node.hpp"

#include <any>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <imgui.h>

namespace saida {

// Inspector helper that turns live ImGui edits into one undoable command.
// Commands re-resolve nodes by id, so they survive scene reconstruction.
class PropertyEditor {
public:
    PropertyEditor(EditorUI& ui, Node* node) : ui_(ui), node_(node) {}

    void dragFloat(const char* label, std::function<float(Node&)> get,
                   std::function<void(Node&, float)> set,
                   float speed, float mn = 0.0f, float mx = 0.0f) {
        edit<float>(label, std::move(get), std::move(set),
                    [&](float& v) { return ImGui::DragFloat(label, &v, speed, mn, mx); });
    }

    void dragInt(const char* label, std::function<int(Node&)> get,
                 std::function<void(Node&, int)> set,
                 float speed = 1.0f, int mn = 0, int mx = 0) {
        edit<int>(label, std::move(get), std::move(set),
                  [&](int& v) { return ImGui::DragInt(label, &v, speed, mn, mx); });
    }

    void sliderFloat(const char* label, std::function<float(Node&)> get,
                     std::function<void(Node&, float)> set, float mn, float mx) {
        edit<float>(label, std::move(get), std::move(set),
                    [&](float& v) { return ImGui::SliderFloat(label, &v, mn, mx); });
    }

    void sliderAngle(const char* label, std::function<float(Node&)> get,
                     std::function<void(Node&, float)> set) {
        edit<float>(label, std::move(get), std::move(set),
                    [&](float& v) { return ImGui::SliderAngle(label, &v); });
    }

    void dragFloat3(const char* label, std::function<glm::vec3(Node&)> get,
                    std::function<void(Node&, glm::vec3)> set,
                    float speed, float mn = 0.0f, float mx = 0.0f) {
        edit<glm::vec3>(label, std::move(get), std::move(set),
                        [&](glm::vec3& v) { return ImGui::DragFloat3(label, &v.x, speed, mn, mx); });
    }

    void sliderFloat3(const char* label, std::function<glm::vec3(Node&)> get,
                      std::function<void(Node&, glm::vec3)> set, float mn, float mx) {
        edit<glm::vec3>(label, std::move(get), std::move(set),
                        [&](glm::vec3& v) { return ImGui::SliderFloat3(label, &v.x, mn, mx); });
    }

    void dragFloat4(const char* label, std::function<glm::vec4(Node&)> get,
                    std::function<void(Node&, glm::vec4)> set,
                    float speed, float mn = 0.0f, float mx = 0.0f) {
        edit<glm::vec4>(label, std::move(get), std::move(set),
                        [&](glm::vec4& v) { return ImGui::DragFloat4(label, &v.x, speed, mn, mx); });
    }

    void colorEdit3(const char* label, std::function<glm::vec3(Node&)> get,
                    std::function<void(Node&, glm::vec3)> set) {
        edit<glm::vec3>(label, std::move(get), std::move(set),
                        [&](glm::vec3& v) { return ImGui::ColorEdit3(label, &v.x); });
    }

    void colorEdit4(const char* label, std::function<glm::vec4(Node&)> get,
                    std::function<void(Node&, glm::vec4)> set) {
        edit<glm::vec4>(label, std::move(get), std::move(set),
                        [&](glm::vec4& v) { return ImGui::ColorEdit4(label, &v.x); });
    }

    void checkbox(const char* label, std::function<bool(Node&)> get,
                  std::function<void(Node&, bool)> set) {
        edit<bool>(label, std::move(get), std::move(set),
                   [&](bool& v) { return ImGui::Checkbox(label, &v); });
    }

    void combo(const char* label, std::function<int(Node&)> get,
               std::function<void(Node&, int)> set, const char* const items[], int count) {
        edit<int>(label, std::move(get), std::move(set),
                  [&](int& v) { return ImGui::Combo(label, &v, items, count); });
    }

    void combo(const char* label, std::function<int(Node&)> get,
               std::function<void(Node&, int)> set, const char* itemsZeroSep) {
        edit<int>(label, std::move(get), std::move(set),
                  [&](int& v) { return ImGui::Combo(label, &v, itemsZeroSep); });
    }

    // Commit text only when editing ends; setters may have side effects.
    void inputText(const char* label, std::function<std::string(Node&)> get,
                   std::function<void(Node&, std::string)> set) {
        if (!node_) return;
        const std::string current = get(*node_);
        char buf[512];
        std::strncpy(buf, current.c_str(), sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        ImGui::InputText(label, buf, sizeof(buf));
        commit<std::string>(label, current, std::string(buf), set);
    }

    // One-shot property change for drag-drop, combo, or button edits.
    template <class T>
    void push(const char* label, std::function<T(Node&)> get,
              std::function<void(Node&, T)> set, const T& newValue) {
        if (!node_) return;
        const T before = get(*node_);
        const NodeId nodeId = node_->id();
        ui_.execute(std::make_unique<SetPropertyCommand>(
            nodeId, std::string(label),
            [set, before](Node& n) { set(n, before); },
            [set, newValue](Node& n) { set(n, newValue); }));
    }

private:
    // Apply live for feedback; record undo only on commit.
    template <class T, class Widget>
    void edit(const char* label, std::function<T(Node&)> get,
              std::function<void(Node&, T)> set, Widget&& widget) {
        if (!node_) return;
        T value = get(*node_);
        const T before = value;
        const bool changed = widget(value);
        if (changed) set(*node_, value);  // immediate visual feedback
        commit<T>(label, before, value, set);
    }

    // Capture the pre-edit value when the widget activates; emit one command
    // when it deactivates after an actual edit.
    template <class T>
    void commit(const char* label, const T& before, const T& after,
                const std::function<void(Node&, T)>& set) {
        const ImGuiID id = ImGui::GetItemID();
        if (ImGui::IsItemActivated()) {
            ui_.propEditId_ = id;
            ui_.propEditOld_ = before;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && ui_.propEditId_ == id) {
            T oldValue = before;
            if (ui_.propEditOld_.has_value())
                oldValue = std::any_cast<T>(ui_.propEditOld_);
            const NodeId nodeId = node_->id();
            ui_.execute(std::make_unique<SetPropertyCommand>(
                nodeId, std::string(label),
                [set, oldValue](Node& n) { set(n, oldValue); },
                [set, newValue = after](Node& n) { set(n, newValue); }));
            ui_.propEditId_ = 0;
            ui_.propEditOld_.reset();
        }
    }

    EditorUI& ui_;
    Node* node_;
};

} // namespace saida
