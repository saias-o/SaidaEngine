#include "editor/panels/InspectorPanel.hpp"
#include "editor/EditorUI.hpp"
#include "editor/InspectorRegistry.hpp"
#include "editor/PropertyEditor.hpp"
#include "core/Reflection.hpp"
#include "scene/Scene.hpp"
#include "scene/Node.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/CameraNode.hpp"
#include "physics/CollisionObjectNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/AreaNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "graphics/ResourceManager.hpp"
#include "project/Project.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "project/AssetRegistry.hpp"

#include "scene/BehaviourRegistry.hpp"
#include "scene/BVHLoader.hpp"
#include "behaviours/LODGroupBehaviour.hpp"
#include "scene/animation/Animator.hpp"
#include "scene/animation/AnimationClip.hpp"

#include "nodes/UICanvasNode.hpp"
#include "nodes/UIColorNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "nodes/UITextNode.hpp"
#include "nodes/UIButtonNode.hpp"
#include "nodes/UIToggleNode.hpp"
#include "nodes/WebCanvasNode.hpp"

#include <imgui.h>
#include <array>
#include <cstring>
#include <algorithm>
#include <functional>
#include <utility>

namespace saida {

namespace {
    std::string getAssetName(AssetID id, EditorUI* editor) {
        if (id == kAssetInvalid) return "None";
        if (editor->ctxProject()) {
            std::string path = editor->ctxProject()->assetRegistry().getPath(id);
            if (!path.empty()) {
                size_t slash = path.find_last_of("/\\");
                if (slash != std::string::npos) return path.substr(slash + 1);
                return path;
            }
        }
        return "Assigned";
    }

    // Generic inspector for reflected properties. PropertyDesc lives in the
    // global TypeRegistry, so capturing it by pointer is safe.
    void drawReflectedProperties(const reflect::TypeDesc& td, Node* node, EditorUI* editor) {
        if (td.properties.empty()) return;
        PropertyEditor pe(*editor, node);
        for (const reflect::PropertyDesc& p : td.properties) {
            const reflect::PropertyDesc* pd = &p;
            const char* label = p.name.c_str();
            const bool hasRange = p.hasRange;
            const float mn = static_cast<float>(p.min), mx = static_cast<float>(p.max);

            if (p.kind == "float") {
                auto get = [pd](Node& n) { nlohmann::json j; pd->get(&n, j); return j.is_number() ? j.get<float>() : 0.0f; };
                auto set = [pd](Node& n, float v) { nlohmann::json j = v; pd->set(&n, j); };
                if (hasRange) pe.sliderFloat(label, get, set, mn, mx);
                else pe.dragFloat(label, get, set, 0.05f);
            } else if (p.kind == "int") {
                auto get = [pd](Node& n) { nlohmann::json j; pd->get(&n, j); return j.is_number() ? j.get<int>() : 0; };
                auto set = [pd](Node& n, int v) { nlohmann::json j = v; pd->set(&n, j); };
                pe.dragInt(label, get, set, 1.0f, hasRange ? static_cast<int>(mn) : 0, hasRange ? static_cast<int>(mx) : 0);
            } else if (p.kind == "bool") {
                pe.checkbox(label,
                    [pd](Node& n) { nlohmann::json j; pd->get(&n, j); return j.is_boolean() && j.get<bool>(); },
                    [pd](Node& n, bool v) { nlohmann::json j = v; pd->set(&n, j); });
            } else if (p.kind == "vec3") {
                auto get = [pd](Node& n) { nlohmann::json j; pd->get(&n, j); glm::vec3 v(0.0f);
                    if (j.is_array() && j.size() >= 3) { v.x = j[0]; v.y = j[1]; v.z = j[2]; } return v; };
                auto set = [pd](Node& n, glm::vec3 v) { pd->set(&n, nlohmann::json::array({v.x, v.y, v.z})); };
                const bool isColor = p.name.find("olor") != std::string::npos;  // "color"/"Color"
                if (isColor) pe.colorEdit3(label, get, set);
                else pe.dragFloat3(label, get, set, 0.05f);
            } else if (p.kind == "vec4") {
                auto get = [pd](Node& n) { nlohmann::json j; pd->get(&n, j); glm::vec4 v(0.0f);
                    if (j.is_array() && j.size() >= 4) { v.x = j[0]; v.y = j[1]; v.z = j[2]; v.w = j[3]; } return v; };
                auto set = [pd](Node& n, glm::vec4 v) { pd->set(&n, nlohmann::json::array({v.x, v.y, v.z, v.w})); };
                const bool isColor = p.name.find("olor") != std::string::npos;
                if (isColor) pe.colorEdit4(label, get, set);
                else pe.dragFloat4(label, get, set, 0.05f);
            } else if (p.kind == "string") {
                pe.inputText(label,
                    [pd](Node& n) { nlohmann::json j; pd->get(&n, j); return j.is_string() ? j.get<std::string>() : std::string(); },
                    [pd](Node& n, std::string v) { nlohmann::json j = v; pd->set(&n, j); });
            } else if (p.kind == "enum") {
                auto get = [pd](Node& n) { nlohmann::json j; pd->get(&n, j); return j.is_number_integer() ? j.get<int>() : 0; };
                auto set = [pd](Node& n, int v) { nlohmann::json j = v; pd->set(&n, j); };
                if (!p.enumLabels.empty()) {
                    std::vector<const char*> labels;
                    labels.reserve(p.enumLabels.size());
                    for (const std::string& item : p.enumLabels) labels.push_back(item.c_str());
                    pe.combo(label, get, set, labels.data(), static_cast<int>(labels.size()));
                } else {
                    pe.dragInt(label, get, set);
                }
            }
            if (!p.tooltip.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", p.tooltip.c_str());
        }
    }

    size_t behaviourTypeIndex(const Node* node, const Behaviour* target) {
        if (!node || !target || !target->typeName()) return 0;
        size_t index = 0;
        for (const auto& b : node->behaviours()) {
            if (!b->typeName() || std::strcmp(b->typeName(), target->typeName()) != 0) continue;
            if (b.get() == target) return index;
            ++index;
        }
        return 0;
    }

    Behaviour* behaviourByTypeIndex(Node& node, const std::string& type, size_t index) {
        size_t seen = 0;
        for (const auto& b : node.behaviours()) {
            if (!b->typeName() || type != b->typeName()) continue;
            if (seen++ == index) return b.get();
        }
        return nullptr;
    }

    nlohmann::json readProperty(const reflect::PropertyDesc& p, const void* obj) {
        nlohmann::json value;
        p.get(obj, value);
        return value;
    }

    void applyBehaviourProperty(Node& node, const std::string& type, size_t index,
                                const std::string& property, const nlohmann::json& value) {
        Behaviour* b = behaviourByTypeIndex(node, type, index);
        if (!b) return;
        if (const reflect::TypeDesc* td = reflect::TypeRegistry::instance().find(type)) {
            if (const reflect::PropertyDesc* p = td->findProperty(property))
                p->set(b, value);
        }
    }

    void applyBehaviourEnabled(Node& node, const std::string& type, size_t index, bool enabled) {
        if (Behaviour* b = behaviourByTypeIndex(node, type, index))
            b->setEnabled(enabled);
    }

    bool editReflectedJson(const reflect::PropertyDesc& p, nlohmann::json& value) {
        const char* label = p.name.c_str();
        const bool hasRange = p.hasRange;
        const float mn = static_cast<float>(p.min);
        const float mx = static_cast<float>(p.max);

        if (p.kind == "float") {
            float v = value.is_number() ? value.get<float>() : 0.0f;
            bool changed = hasRange ? ImGui::SliderFloat(label, &v, mn, mx)
                                    : ImGui::DragFloat(label, &v, 0.05f);
            if (changed) value = v;
            return changed;
        }
        if (p.kind == "int") {
            int v = value.is_number_integer() ? value.get<int>() : 0;
            bool changed = ImGui::DragInt(label, &v, 1.0f,
                hasRange ? static_cast<int>(mn) : 0,
                hasRange ? static_cast<int>(mx) : 0);
            if (changed) value = v;
            return changed;
        }
        if (p.kind == "bool") {
            bool v = value.is_boolean() && value.get<bool>();
            bool changed = ImGui::Checkbox(label, &v);
            if (changed) value = v;
            return changed;
        }
        if (p.kind == "vec3") {
            glm::vec3 v(0.0f);
            if (value.is_array() && value.size() >= 3)
                v = {value[0].get<float>(), value[1].get<float>(), value[2].get<float>()};
            const bool isColor = p.name.find("olor") != std::string::npos;
            bool changed = isColor ? ImGui::ColorEdit3(label, &v.x)
                                   : ImGui::DragFloat3(label, &v.x, 0.05f);
            if (changed) value = nlohmann::json::array({v.x, v.y, v.z});
            return changed;
        }
        if (p.kind == "vec4") {
            glm::vec4 v(0.0f);
            if (value.is_array() && value.size() >= 4)
                v = {value[0].get<float>(), value[1].get<float>(),
                     value[2].get<float>(), value[3].get<float>()};
            const bool isColor = p.name.find("olor") != std::string::npos;
            bool changed = isColor ? ImGui::ColorEdit4(label, &v.x)
                                   : ImGui::DragFloat4(label, &v.x, 0.05f);
            if (changed) value = nlohmann::json::array({v.x, v.y, v.z, v.w});
            return changed;
        }
        if (p.kind == "string" || p.kind == "asset") {
            std::array<char, 512> buf{};
            std::string s = value.is_string() ? value.get<std::string>() : std::string();
            std::strncpy(buf.data(), s.c_str(), buf.size() - 1);
            bool changed = ImGui::InputText(label, buf.data(), buf.size());
            if (changed) value = std::string(buf.data());
            return changed;
        }
        if (p.kind == "enum") {
            int v = value.is_number_integer() ? value.get<int>() : 0;
            bool changed = false;
            if (!p.enumLabels.empty()) {
                std::vector<const char*> labels;
                labels.reserve(p.enumLabels.size());
                for (const std::string& item : p.enumLabels) labels.push_back(item.c_str());
                changed = ImGui::Combo(label, &v, labels.data(), static_cast<int>(labels.size()));
            } else {
                changed = ImGui::DragInt(label, &v);
            }
            if (changed) value = v;
            return changed;
        }
        return false;
    }

    std::unique_ptr<Command> drawReflectedBehaviourProperties(const reflect::TypeDesc& td, Node* node,
                                                              Behaviour* behaviour) {
        if (!node || !behaviour || td.properties.empty() || !behaviour->typeName()) return nullptr;
        const std::string type = behaviour->typeName();
        const size_t index = behaviourTypeIndex(node, behaviour);
        static ImGuiID editId = 0;
        static nlohmann::json editOld;

        for (const reflect::PropertyDesc& p : td.properties) {
            nlohmann::json value = readProperty(p, behaviour);
            const nlohmann::json before = value;
            const bool changed = editReflectedJson(p, value);
            if (changed) p.set(behaviour, value);

            const ImGuiID id = ImGui::GetItemID();
            if (ImGui::IsItemActivated()) {
                editId = id;
                editOld = before;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && editId == id) {
                const nlohmann::json oldValue = editOld;
                const nlohmann::json newValue = value;
                const std::string prop = p.name;
                editId = 0;
                editOld = nullptr;
                return std::make_unique<SetPropertyCommand>(
                    node->id(), "Set " + type + "." + prop,
                    [type, index, prop, oldValue](Node& n) {
                        applyBehaviourProperty(n, type, index, prop, oldValue);
                    },
                    [type, index, prop, newValue](Node& n) {
                        applyBehaviourProperty(n, type, index, prop, newValue);
                    });
            }
            if (!p.tooltip.empty() && ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", p.tooltip.c_str());
        }
        return nullptr;
    }
}

void InspectorPanel::draw(EditorUI* editor) {
    ImGui::Begin("Inspector", &editor->showInspector_);

    if (!editor->selectedNode_) {
        ImGui::TextDisabled("No node selected");
        ImGui::End();
        return;
    }

    Node* node = editor->selectedNode_;

    // Play is inspectable but read-only: show the live values, disable edits.
    const bool readOnly = !editor->canEdit();
    if (readOnly)
        ImGui::TextDisabled("Play mode — inspecting (read-only)");
    ImGui::BeginDisabled(readOnly);

    drawNodeHeader(node, editor);

    if (auto* uiNode = dynamic_cast<UINode*>(node)) {
        drawUINode(uiNode, editor);
        PropertyEditor pe(*editor, node);
        if (auto* colorNode = dynamic_cast<UIColorNode*>(uiNode)) {
            (void)colorNode;
            ImGui::SeparatorText("UIColorNode");
            pe.colorEdit4("Color##UIColor",
                          [](Node& n) { return static_cast<UIColorNode&>(n).color(); },
                          [](Node& n, glm::vec4 v) { static_cast<UIColorNode&>(n).setColor(v); });
        }
        else if (auto* imageNode = dynamic_cast<UIImageNode*>(uiNode)) {
            ImGui::SeparatorText("UIImageNode");
            std::string texName = "Image Texture [" + getAssetName(imageNode->texture(), editor) + "]";
            if (imageNode->texture() == kAssetInvalid) texName = "Drop Texture Here";
            ImGui::Button(texName.c_str(), ImVec2(-FLT_MIN, 30));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
                    AssetID id = *(AssetID*)payload->Data;
                    pe.push<AssetID>("Image Texture",
                                     [](Node& n) { return static_cast<UIImageNode&>(n).texture(); },
                                     [](Node& n, AssetID a) { static_cast<UIImageNode&>(n).setTexture(a); },
                                     id);
                }
                ImGui::EndDragDropTarget();
            }
        }
        else if (auto* textNode = dynamic_cast<UITextNode*>(uiNode)) {
            (void)textNode;
            ImGui::SeparatorText("UITextNode");
            pe.inputText("Text",
                         [](Node& n) { return static_cast<UITextNode&>(n).text(); },
                         [](Node& n, std::string v) { static_cast<UITextNode&>(n).setText(v); });
            pe.dragFloat("Font Size",
                         [](Node& n) { return static_cast<UITextNode&>(n).fontSize(); },
                         [](Node& n, float v) { static_cast<UITextNode&>(n).setFontSize(v); }, 0.5f, 1.0f, 128.0f);
            pe.colorEdit4("Color##UIText",
                          [](Node& n) { return static_cast<UITextNode&>(n).color(); },
                          [](Node& n, glm::vec4 v) { static_cast<UITextNode&>(n).setColor(v); });
        }

        if (auto* interactable = dynamic_cast<UIInteractableNode*>(uiNode)) {
            (void)interactable;
            ImGui::SeparatorText("UI Interactable");
            pe.checkbox("Interactable",
                        [](Node& n) { return static_cast<UIInteractableNode&>(n).interactable(); },
                        [](Node& n, bool v) { static_cast<UIInteractableNode&>(n).setInteractable(v); });

            if (auto* btn = dynamic_cast<UIButtonNode*>(interactable)) {
                (void)btn;
                // setColors is a bundle; each edit re-reads the siblings so the
                // command captures only the changed channel.
                pe.colorEdit4("Normal Color",
                              [](Node& n) { return static_cast<UIButtonNode&>(n).normalColor(); },
                              [](Node& n, glm::vec4 v) { auto& b = static_cast<UIButtonNode&>(n); b.setColors(v, b.hoverColor(), b.pressedColor()); });
                pe.colorEdit4("Hover Color",
                              [](Node& n) { return static_cast<UIButtonNode&>(n).hoverColor(); },
                              [](Node& n, glm::vec4 v) { auto& b = static_cast<UIButtonNode&>(n); b.setColors(b.normalColor(), v, b.pressedColor()); });
                pe.colorEdit4("Pressed Color",
                              [](Node& n) { return static_cast<UIButtonNode&>(n).pressedColor(); },
                              [](Node& n, glm::vec4 v) { auto& b = static_cast<UIButtonNode&>(n); b.setColors(b.normalColor(), b.hoverColor(), v); });
            }
            else if (auto* tgl = dynamic_cast<UIToggleNode*>(interactable)) {
                (void)tgl;
                pe.checkbox("Is On",
                            [](Node& n) { return static_cast<UIToggleNode&>(n).isOn(); },
                            [](Node& n, bool v) { static_cast<UIToggleNode&>(n).setIsOn(v); });
                pe.colorEdit4("On Color",
                              [](Node& n) { return static_cast<UIToggleNode&>(n).onColor(); },
                              [](Node& n, glm::vec4 v) { auto& t = static_cast<UIToggleNode&>(n); t.setColors(v, t.offColor()); });
                pe.colorEdit4("Off Color",
                              [](Node& n) { return static_cast<UIToggleNode&>(n).offColor(); },
                              [](Node& n, glm::vec4 v) { auto& t = static_cast<UIToggleNode&>(n); t.setColors(t.onColor(), v); });
            }
        }
    } else if (auto* webNode = dynamic_cast<WebCanvasNode*>(node)) {
        ImGui::SeparatorText("Web Canvas");
        PropertyEditor pe(*editor, node);

        pe.dragInt("Width",
                   [](Node& n) { return static_cast<int>(static_cast<WebCanvasNode&>(n).width()); },
                   [](Node& n, int v) {
                       auto& w = static_cast<WebCanvasNode&>(n);
                       w.resize(static_cast<uint32_t>(std::max(v, 1)), w.height());
                   },
                   1, 1, 8192);
        pe.dragInt("Height",
                   [](Node& n) { return static_cast<int>(static_cast<WebCanvasNode&>(n).height()); },
                   [](Node& n, int v) {
                       auto& w = static_cast<WebCanvasNode&>(n);
                       w.resize(w.width(), static_cast<uint32_t>(std::max(v, 1)));
                   },
                   1, 1, 8192);

        pe.combo("Mode",
                 [](Node& n) { return static_cast<int>(static_cast<WebCanvasNode&>(n).mode()); },
                 [](Node& n, int v) { static_cast<WebCanvasNode&>(n).setMode(static_cast<WebCanvasNode::Mode>(v)); },
                 "ScreenSpace\0WorldSpace\0");

        pe.inputText("URL",
                     [](Node& n) { return static_cast<WebCanvasNode&>(n).url(); },
                     [](Node& n, std::string v) { static_cast<WebCanvasNode&>(n).setUrl(v); });

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_HTML")) {
                const char* path = (const char*)payload->Data;
                std::string url = "file:///" + std::string(path);
                std::replace(url.begin(), url.end(), '\\', '/');
                pe.push<std::string>("Set WebCanvas URL",
                    [](Node& n) { return static_cast<WebCanvasNode&>(n).url(); },
                    [](Node& n, std::string v) { static_cast<WebCanvasNode&>(n).setUrl(v); },
                    url);
            }
            ImGui::EndDragDropTarget();
        }

        if (ImGui::Button("Reload Page", ImVec2(-FLT_MIN, 30))) {
            webNode->reload();
        }
        pe.checkbox("Hot Reload",
                    [](Node& n) { return static_cast<WebCanvasNode&>(n).hotReloadEnabled(); },
                    [](Node& n, bool v) { static_cast<WebCanvasNode&>(n).setHotReloadEnabled(v); });
        pe.checkbox("Interactive",
                    [](Node& n) { return static_cast<WebCanvasNode&>(n).interactive(); },
                    [](Node& n, bool v) { static_cast<WebCanvasNode&>(n).setInteractive(v); });
        pe.dragFloat("World Width",
                     [](Node& n) { return static_cast<WebCanvasNode&>(n).worldWidth(); },
                     [](Node& n, float v) { static_cast<WebCanvasNode&>(n).setWorldWidth(v); },
                     0.01f, 0.01f, 100.0f);
        pe.dragInt("Render Order",
                   [](Node& n) { return static_cast<WebCanvasNode&>(n).renderOrder(); },
                   [](Node& n, int v) { static_cast<WebCanvasNode&>(n).setRenderOrder(v); },
                   1, -1024, 1024);
        if (!webNode->lastLoadOk()) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "Last error: %s", webNode->lastError().c_str());
        } else {
            ImGui::TextDisabled("Last load: OK");
        }
        ImGui::Separator();
        ImGui::Text("Startup Scripts");

        // Every list mutation is one undoable command on a full-list snapshot:
        // element indices then stay meaningful across undo/redo (LIFO restores
        // the exact list an edit was made against).
        const auto scriptsGet = [](Node& n) -> std::vector<std::string> {
            return static_cast<WebCanvasNode&>(n).startupScripts();
        };
        const auto scriptsSet = [](Node& n, std::vector<std::string> v) {
            static_cast<WebCanvasNode&>(n).setStartupScripts(std::move(v));
        };

        static ImGuiID scriptEditId = 0;
        static std::vector<std::string> scriptEditOld;
        const std::vector<std::string>& scripts = webNode->startupScripts();
        int removeIndex = -1;
        for (size_t i = 0; i < scripts.size(); ++i) {
            ImGui::PushID(static_cast<int>(i));

            char buffer[1024];
            strncpy(buffer, scripts[i].c_str(), sizeof(buffer) - 1);
            buffer[sizeof(buffer) - 1] = '\0';

            if (ImGui::InputText("##script", buffer, sizeof(buffer))) {
                std::vector<std::string> edited = scripts;
                edited[i] = buffer;
                webNode->setStartupScripts(std::move(edited));  // live feedback
            }
            const ImGuiID id = ImGui::GetItemID();
            if (ImGui::IsItemActivated()) {
                scriptEditId = id;
                scriptEditOld = scripts;
            }
            if (ImGui::IsItemDeactivatedAfterEdit() && scriptEditId == id) {
                const std::vector<std::string> oldScripts = scriptEditOld;
                const std::vector<std::string> newScripts = webNode->startupScripts();
                scriptEditId = 0;
                scriptEditOld.clear();
                const NodeId nodeId = webNode->id();
                editor->execute(std::make_unique<SetPropertyCommand>(
                    nodeId, "Edit WebCanvas Script",
                    [scriptsSet, oldScripts](Node& n) { scriptsSet(n, oldScripts); },
                    [scriptsSet, newScripts](Node& n) { scriptsSet(n, newScripts); }));
            }

            ImGui::SameLine();
            if (ImGui::Button("X")) removeIndex = static_cast<int>(i);
            ImGui::PopID();
        }
        if (removeIndex >= 0) {
            std::vector<std::string> without = scripts;
            without.erase(without.begin() + removeIndex);
            pe.push<std::vector<std::string>>("Remove WebCanvas Script",
                                              scriptsGet, scriptsSet, without);
        }

        if (ImGui::Button("Add Script")) {
            std::vector<std::string> withNew = scripts;
            withNew.emplace_back();
            pe.push<std::vector<std::string>>("Add WebCanvas Script",
                                              scriptsGet, scriptsSet, withNew);
        }
        
        ImGui::Separator();
        ImGui::SeparatorText("Execute JS");
        static char jsBuf[1024] = "";
        ImGui::InputTextMultiline("##JS", jsBuf, sizeof(jsBuf), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
        if (ImGui::Button("Run JS", ImVec2(-FLT_MIN, 0))) {
            webNode->executeJS(jsBuf);
        }
    } else {
        // Not a UI node, draw standard transform
        drawTransform(node, editor);
    }

    if (Scene* sceneNode = dynamic_cast<Scene*>(node)) {
        SceneSettings& s = sceneNode->settings();
        PropertyEditor pe(*editor, node);
        // Member-pointer helpers keep SceneSettings edits undoable without
        // per-field boilerplate.
        auto G = [](auto m) { return [m](Node& n) { return static_cast<Scene&>(n).settings().*m; }; };
        auto S = [](auto m) { return [m](Node& n, auto v) { static_cast<Scene&>(n).settings().*m = v; }; };

        // RGB color members are stored as vec4; edit rgb, preserve alpha.
        auto colorRGB = [](glm::vec4 SceneSettings::* m) {
            return std::pair{
                std::function<glm::vec3(Node&)>([m](Node& n) { return glm::vec3(static_cast<Scene&>(n).settings().*m); }),
                std::function<void(Node&, glm::vec3)>([m](Node& n, glm::vec3 v) {
                    glm::vec4& c = static_cast<Scene&>(n).settings().*m; c = glm::vec4(v, c.w); })};
        };

        ImGui::SeparatorText("Scene Settings");
        { auto c = colorRGB(&SceneSettings::ambientLight); pe.colorEdit3("Ambient Light", c.first, c.second); }
        { auto c = colorRGB(&SceneSettings::clearColor); pe.colorEdit3("Clear Color", c.first, c.second); }
        pe.checkbox("Post Processing", G(&SceneSettings::enablePostProcessing), S(&SceneSettings::enablePostProcessing));
        pe.checkbox("Change Rendering At Load", G(&SceneSettings::changeRenderingAtLoad), S(&SceneSettings::changeRenderingAtLoad));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("When loaded as a sub-scene at play time, override the\n"
                              "World's rendering settings (skybox/GI/ambient) with this scene's.");

        ImGui::SeparatorText("Lighting");
        // Radio buttons are instantaneous: record a one-shot command on click.
        int mode = static_cast<int>(s.lightingMode);
        auto modeGet = [](Node& n) { return static_cast<int>(static_cast<Scene&>(n).settings().lightingMode); };
        auto modeSet = [](Node& n, int v) { static_cast<Scene&>(n).settings().lightingMode = static_cast<LightingMode>(v); };
        if (ImGui::RadioButton("Realtime", &mode, static_cast<int>(LightingMode::Realtime)))
            pe.push<int>("Lighting Mode", modeGet, modeSet, static_cast<int>(LightingMode::Realtime));
        ImGui::SameLine();
        if (ImGui::RadioButton("Baked", &mode, static_cast<int>(LightingMode::Baked)))
            pe.push<int>("Lighting Mode", modeGet, modeSet, static_cast<int>(LightingMode::Baked));

        if (s.lightingMode == LightingMode::Realtime) {
            ImGui::TextDisabled("Lighting & shadows computed live; baking disabled.");
        } else {
            if (ImGui::Button("Bake GI"))
                s.bakeRequested = true;  // transient action request, not a document edit
            ImGui::SameLine();
            ImGui::TextDisabled(s.baked ? "GI Bake: frozen" : "GI Bake: none");
        }

        ImGui::SeparatorText("Global Illumination");
        pe.checkbox("Enable GI (indirect diffuse)", G(&SceneSettings::giEnabled), S(&SceneSettings::giEnabled));
        int giMode = static_cast<int>(s.giMode);
        auto giModeGet = [](Node& n) { return static_cast<int>(static_cast<Scene&>(n).settings().giMode); };
        auto giModeSet = [](Node& n, int v) { static_cast<Scene&>(n).settings().giMode = static_cast<GIMode>(v); };
        if (ImGui::RadioButton("Full realtime GI", &giMode, static_cast<int>(GIMode::FullRealtime)))
            pe.push<int>("GI Mode", giModeGet, giModeSet, static_cast<int>(GIMode::FullRealtime));
        ImGui::SameLine();
        if (ImGui::RadioButton("Amortized GI", &giMode, static_cast<int>(GIMode::AmortizedRealtime)))
            pe.push<int>("GI Mode", giModeGet, giModeSet, static_cast<int>(GIMode::AmortizedRealtime));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Updates DDGI every frame during warm-up, then on a cadence.\n"
                              "Higher FPS, but indirect lighting reacts less immediately.");
        pe.sliderFloat("GI Intensity", G(&SceneSettings::giIntensity), S(&SceneSettings::giIntensity), 0.0f, 8.0f);
        pe.checkbox("Debug: show voxel grid", G(&SceneSettings::giDebugVoxels), S(&SceneSettings::giDebugVoxels));

        ImGui::SeparatorText("Debug");
        pe.checkbox("Show skeletons (bone lines)", G(&SceneSettings::showSkeletons), S(&SceneSettings::showSkeletons));

        ImGui::SeparatorText("Skybox");
        std::string skyboxName = "Skybox Texture [" + getAssetName(s.skyboxTexture, editor) + "]";
        if (s.skyboxTexture == kAssetInvalid) skyboxName = "Drop Skybox Texture Here";
        ImGui::Button(skyboxName.c_str(), ImVec2(-FLT_MIN, 30));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
                AssetID id = *(AssetID*)payload->Data;
                pe.push<AssetID>("Skybox Texture", G(&SceneSettings::skyboxTexture), S(&SceneSettings::skyboxTexture), id);
            }
            ImGui::EndDragDropTarget();
        }
        pe.dragFloat("Exposure", G(&SceneSettings::skyboxExposure), S(&SceneSettings::skyboxExposure), 0.05f, 0.0f, 20.0f);
        pe.sliderAngle("Rotation", G(&SceneSettings::skyboxRotation), S(&SceneSettings::skyboxRotation));

        ImGui::SeparatorText("Image-Based Lighting");
        pe.checkbox("Enable IBL", G(&SceneSettings::iblEnabled), S(&SceneSettings::iblEnabled));
        pe.sliderFloat("IBL Diffuse", G(&SceneSettings::iblDiffuseIntensity), S(&SceneSettings::iblDiffuseIntensity), 0.0f, 4.0f);
        pe.sliderFloat("IBL Specular", G(&SceneSettings::iblSpecularIntensity), S(&SceneSettings::iblSpecularIntensity), 0.0f, 4.0f);

        ImGui::SeparatorText("Ambient Occlusion");
        pe.checkbox("Enable AO", G(&SceneSettings::aoEnabled), S(&SceneSettings::aoEnabled));
        pe.sliderFloat("AO Radius", G(&SceneSettings::aoRadius), S(&SceneSettings::aoRadius), 0.05f, 3.0f);
        pe.sliderFloat("AO Intensity", G(&SceneSettings::aoIntensity), S(&SceneSettings::aoIntensity), 0.0f, 3.0f);
        pe.sliderFloat("AO Power", G(&SceneSettings::aoPower), S(&SceneSettings::aoPower), 0.25f, 4.0f);

        ImGui::SeparatorText("Fog");
        pe.checkbox("Enable Fog", G(&SceneSettings::fogEnabled), S(&SceneSettings::fogEnabled));
        { auto c = colorRGB(&SceneSettings::fogColor); pe.colorEdit3("Fog Color", c.first, c.second); }
        pe.sliderFloat("Fog Start", G(&SceneSettings::fogStart), S(&SceneSettings::fogStart), 0.0f, 100.0f);
        pe.sliderFloat("Fog Density", G(&SceneSettings::fogDensity), S(&SceneSettings::fogDensity), 0.0f, 0.25f);

        ImGui::SeparatorText("Bloom");
        pe.checkbox("Enable Bloom", G(&SceneSettings::bloomEnabled), S(&SceneSettings::bloomEnabled));
        pe.sliderFloat("Bloom Threshold", G(&SceneSettings::bloomThreshold), S(&SceneSettings::bloomThreshold), 0.0f, 10.0f);
        pe.sliderFloat("Bloom Intensity", G(&SceneSettings::bloomIntensity), S(&SceneSettings::bloomIntensity), 0.0f, 3.0f);
        pe.sliderFloat("Bloom Radius", G(&SceneSettings::bloomRadius), S(&SceneSettings::bloomRadius), 0.5f, 8.0f);
    }

    if (auto* meshNode = dynamic_cast<MeshNode*>(node)) {
        drawMeshRenderer(meshNode, editor);
        drawMaterial(meshNode->material(), meshNode, editor);
    }

    LightNode* light = node->asLight();
    if (light) {
        ImGui::SeparatorText("Light");

        const char* ltype = light->type == LightType::Directional ? "Directional"
                          : light->type == LightType::Point       ? "Point"
                                                                  : "Spot";
        ImGui::Text("Type: %s", ltype);

        PropertyEditor pe(*editor, node);
        pe.colorEdit3("Color",
                      [](Node& n) { return n.asLight()->color; },
                      [](Node& n, glm::vec3 v) { n.asLight()->color = v; });
        pe.dragFloat("Intensity",
                     [](Node& n) { return n.asLight()->intensity; },
                     [](Node& n, float v) { n.asLight()->intensity = v; }, 0.05f, 0.0f, 20.0f);

        if (light->type == LightType::Directional || light->type == LightType::Spot)
            pe.sliderFloat3("Direction",
                            [](Node& n) { return n.asLight()->direction; },
                            [](Node& n, glm::vec3 v) { n.asLight()->direction = v; }, -1.0f, 1.0f);
        if (light->type == LightType::Point || light->type == LightType::Spot)
            pe.dragFloat("Range",
                         [](Node& n) { return n.asLight()->range; },
                         [](Node& n, float v) { n.asLight()->range = v; }, 0.1f, 0.1f, 100.0f);

        if (light->type == LightType::Spot) {
            pe.dragFloat("Inner Angle",
                         [](Node& n) { return n.asLight()->spotInnerAngle; },
                         [](Node& n, float v) {
                             LightNode* l = n.asLight();
                             l->spotInnerAngle = std::min(v, l->spotOuterAngle);
                         }, 0.5f, 1.0f, 89.0f);
            pe.dragFloat("Outer Angle",
                         [](Node& n) { return n.asLight()->spotOuterAngle; },
                         [](Node& n, float v) {
                             LightNode* l = n.asLight();
                             l->spotOuterAngle = std::max(v, l->spotInnerAngle);
                         }, 0.5f, 1.0f, 89.0f);
        }

        if (light->type != LightType::Point)
            pe.checkbox("Cast Shadows",
                        [](Node& n) { return n.asLight()->castShadows; },
                        [](Node& n, bool v) { n.asLight()->castShadows = v; });
    }

    if (dynamic_cast<CameraNode*>(node)) {
        ImGui::SeparatorText("Camera");
        PropertyEditor pe(*editor, node);
        pe.dragInt("Priority",
                   [](Node& n) { return static_cast<CameraNode&>(n).priority; },
                   [](Node& n, int v) { static_cast<CameraNode&>(n).priority = v; }, 1.0f, 0, 1000);
        pe.checkbox("Active",
                    [](Node& n) { return static_cast<CameraNode&>(n).active; },
                    [](Node& n, bool v) { static_cast<CameraNode&>(n).active = v; });
        pe.sliderFloat("Field of View",
                       [](Node& n) { return static_cast<CameraNode&>(n).fovDegrees; },
                       [](Node& n, float v) { static_cast<CameraNode&>(n).fovDegrees = v; }, 20.0f, 120.0f);
        pe.dragFloat("Near",
                     [](Node& n) { return static_cast<CameraNode&>(n).nearZ; },
                     [](Node& n, float v) { static_cast<CameraNode&>(n).nearZ = v; }, 0.01f, 0.01f, 10.0f);
        pe.dragFloat("Far",
                     [](Node& n) { return static_cast<CameraNode&>(n).farZ; },
                     [](Node& n, float v) { static_cast<CameraNode&>(n).farZ = v; }, 1.0f, 1.0f, 10000.0f);
        ImGui::TextDisabled("Highest priority active camera is live (Play).");
    }

    if (auto* particles = dynamic_cast<ParticleSystemNode*>(node)) {
        ImGui::SeparatorText("SaidaFX");
        PropertyEditor pe(*editor, node);
        if (ImGui::Button("Apply Effect Preset", ImVec2(-FLT_MIN, 0.0f))) {
            particles->applyEffectPreset();
            editor->markDirty();
        }
        if (ImGui::Button("Load Effect", ImVec2(-FLT_MIN, 0.0f))) {
            particles->loadEffect();
            editor->markDirty();
        }
        ImGui::Button("Drop .saidafx Here", ImVec2(-FLT_MIN, 28.0f));
        if (editor->ctxProject() && ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
                AssetID id = *(AssetID*)payload->Data;
                auto& registry = editor->ctxProject()->assetRegistry();
                if (registry.getType(id) == AssetType::Effect) {
                    std::string path = registry.getAbsolutePath(id);
                    pe.push<std::string>("Effect Path",
                        [](Node& n) { return static_cast<ParticleSystemNode&>(n).effectPath; },
                        [](Node& n, std::string v) { static_cast<ParticleSystemNode&>(n).effectPath = std::move(v); },
                        path);
                    particles->loadEffect();
                    editor->markDirty();
                }
            }
            ImGui::EndDragDropTarget();
        }
        const float overdrawEstimate = static_cast<float>(std::max(0, particles->maxParticles)) *
            particles->startSize * particles->startSize * std::max(1.0f, particles->stretch);
        ImGui::Text("Budget: %d particles", particles->maxParticles);
        if (particles->maxParticles > 1024) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Mobile/XR warning: high particle count");
        }
        if (overdrawEstimate > 180.0f) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Overdraw warning: large dense billboards");
        }
        if (particles->blendMode == ParticleSystemNode::BlendMode::Alpha &&
            particles->maxParticles > 512 && particles->startSize > 0.25f) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Alpha warning: consider lower spawn/budget for VR");
        }
        ImGui::TextDisabled("Preset or .saidafx fills the editable parameters below.");
    }

    // Generic fallback: any reflected node type (e.g. WaterNode) gets a full
    // inspector from its manifest, no per-type editor code. Skip LightNode, which
    // has a tailored UI above.
    if (!node->asLight()) {
        if (const reflect::TypeDesc* td = reflect::TypeRegistry::instance().find(node->typeName())) {
            ImGui::SeparatorText(node->typeName());
            drawReflectedProperties(*td, node, editor);
        }
    }

    if (auto* body = node->asCollisionObject())
        drawPhysicsBody(body, editor);
    if (auto* shape = dynamic_cast<CollisionShapeNode*>(node))
        drawCollisionShape(shape, editor);

    drawBehaviours(node, editor);

    ImGui::EndDisabled();

    ImGui::End();
}

// Walk up to the body that owns this shape and force a rebuild so edits apply.
static void markOwningBodyDirty(Node* node) {
    for (Node* p = node->parent(); p; p = p->parent()) {
        if (auto* body = p->asCollisionObject()) { body->markDirty(); return; }
    }
}

void InspectorPanel::drawPhysicsBody(CollisionObjectNode* body, EditorUI* editor) {
    ImGui::SeparatorText("Physics Body");

    // Every setter rebuilds the body so creation-time params take effect — on
    // undo too, since the command replays the same setter with the old value.
    PropertyEditor pe(*editor, body);
    auto rebuild = [](Node& n) { if (auto* b = n.asCollisionObject()) b->markDirty(); };

    pe.dragFloat("Friction",
                 [](Node& n) { return n.asCollisionObject()->friction; },
                 [rebuild](Node& n, float v) { n.asCollisionObject()->friction = v; rebuild(n); }, 0.01f, 0.0f, 2.0f);
    pe.dragFloat("Restitution",
                 [](Node& n) { return n.asCollisionObject()->restitution; },
                 [rebuild](Node& n, float v) { n.asCollisionObject()->restitution = v; rebuild(n); }, 0.01f, 0.0f, 1.0f);

    if (auto* rb = dynamic_cast<RigidBodyNode*>(body)) {
        (void)rb;
        pe.checkbox("Kinematic",
                    [](Node& n) { return static_cast<RigidBodyNode&>(n).kinematic; },
                    [rebuild](Node& n, bool v) { static_cast<RigidBodyNode&>(n).kinematic = v; rebuild(n); });
        pe.dragFloat("Mass",
                     [](Node& n) { return static_cast<RigidBodyNode&>(n).mass; },
                     [rebuild](Node& n, float v) { static_cast<RigidBodyNode&>(n).mass = v; rebuild(n); }, 0.05f, 0.0f, 1000.0f);
        pe.dragFloat("Gravity Factor",
                     [](Node& n) { return static_cast<RigidBodyNode&>(n).gravityFactor; },
                     [rebuild](Node& n, float v) { static_cast<RigidBodyNode&>(n).gravityFactor = v; rebuild(n); }, 0.05f, 0.0f, 4.0f);
        pe.dragFloat("Linear Damping",
                     [](Node& n) { return static_cast<RigidBodyNode&>(n).linearDamping; },
                     [rebuild](Node& n, float v) { static_cast<RigidBodyNode&>(n).linearDamping = v; rebuild(n); }, 0.005f, 0.0f, 1.0f);
        pe.dragFloat("Angular Damping",
                     [](Node& n) { return static_cast<RigidBodyNode&>(n).angularDamping; },
                     [rebuild](Node& n, float v) { static_cast<RigidBodyNode&>(n).angularDamping = v; rebuild(n); }, 0.005f, 0.0f, 1.0f);
    } else if (auto* area = dynamic_cast<AreaNode*>(body)) {
        (void)area;
        pe.checkbox("Moving (kinematic trigger)",
                    [](Node& n) { return static_cast<AreaNode&>(n).moving; },
                    [rebuild](Node& n, bool v) { static_cast<AreaNode&>(n).moving = v; rebuild(n); });
    } else if (auto* ch = dynamic_cast<CharacterBodyNode*>(body)) {
        pe.dragFloat("Mass",
                     [](Node& n) { return static_cast<CharacterBodyNode&>(n).mass; },
                     [rebuild](Node& n, float v) { static_cast<CharacterBodyNode&>(n).mass = v; rebuild(n); }, 0.5f, 1.0f, 500.0f);
        pe.dragFloat("Max Slope Angle",
                     [](Node& n) { return static_cast<CharacterBodyNode&>(n).maxSlopeAngle; },
                     [rebuild](Node& n, float v) { static_cast<CharacterBodyNode&>(n).maxSlopeAngle = v; rebuild(n); }, 0.5f, 0.0f, 89.0f);
        ImGui::TextDisabled("On floor: %s", ch->isOnFloor() ? "yes" : "no");
    }
}

void InspectorPanel::drawCollisionShape(CollisionShapeNode* shape, EditorUI* editor) {
    ImGui::SeparatorText("Collision Shape");

    PropertyEditor pe(*editor, shape);
    // Editing a shape field rebuilds the owning body (on undo too).
    auto rebuild = [](Node& n) { markOwningBodyDirty(&n); };

    // A shape-type change and "Recompute" both funnel through Auto detection,
    // which rewrites the manual parameters; their commands therefore snapshot
    // the full durable state (type + parameters), and replaying a state that
    // enters Auto re-arms detection (resetAuto) exactly like the live edit.
    struct ShapeState {
        CollisionShapeType type;
        glm::vec3 halfExtents;
        float radius;
        float height;
        int axis;
        glm::vec3 offset;
    };
    const auto captureShape = [](const CollisionShapeNode& s) {
        return ShapeState{s.shapeType, s.halfExtents, s.radius, s.height, s.axis, s.offset};
    };
    const auto applyShape = [](Node& n, const ShapeState& s, bool rearmAuto) {
        auto& node = static_cast<CollisionShapeNode&>(n);
        node.shapeType = s.type;
        node.halfExtents = s.halfExtents;
        node.radius = s.radius;
        node.height = s.height;
        node.axis = s.axis;
        node.offset = s.offset;
        if (rearmAuto && s.type == CollisionShapeType::Auto) node.resetAuto();
        markOwningBodyDirty(&node);
    };

    const char* kinds[] = {"Auto", "Box", "Sphere", "Capsule", "ConvexHull", "Mesh"};
    int current = static_cast<int>(shape->shapeType);
    if (ImGui::Combo("Shape", &current, kinds, IM_ARRAYSIZE(kinds)) &&
        current != static_cast<int>(shape->shapeType)) {
        ShapeState before = captureShape(*shape);
        ShapeState after = before;
        after.type = static_cast<CollisionShapeType>(current);
        editor->execute(std::make_unique<SetPropertyCommand>(
            shape->id(), "Set Collision Shape Type",
            // Undo re-applies the captured parameters without re-arming Auto:
            // the frozen detection stays frozen on the restored values.
            [applyShape, before](Node& n) { applyShape(n, before, false); },
            [applyShape, after](Node& n) { applyShape(n, after, true); }));
    }

    if (shape->shapeType == CollisionShapeType::Auto) {
        ImGui::TextDisabled("Detected: %s (frozen)", toString(shape->resolvedType()));
        if (ImGui::Button("Recompute from mesh")) {
            ShapeState before = captureShape(*shape);
            editor->execute(std::make_unique<SetPropertyCommand>(
                shape->id(), "Recompute Collision Shape",
                [applyShape, before](Node& n) { applyShape(n, before, false); },
                [applyShape, before](Node& n) { applyShape(n, before, true); }));
        }
    } else if (shape->shapeType == CollisionShapeType::Box) {
        pe.dragFloat3("Half Extents",
                      [](Node& n) { return static_cast<CollisionShapeNode&>(n).halfExtents; },
                      [rebuild](Node& n, glm::vec3 v) { static_cast<CollisionShapeNode&>(n).halfExtents = v; rebuild(n); }, 0.02f, 0.02f, 100.0f);
    } else if (shape->shapeType == CollisionShapeType::Sphere) {
        pe.dragFloat("Radius",
                     [](Node& n) { return static_cast<CollisionShapeNode&>(n).radius; },
                     [rebuild](Node& n, float v) { static_cast<CollisionShapeNode&>(n).radius = v; rebuild(n); }, 0.02f, 0.02f, 100.0f);
    } else if (shape->shapeType == CollisionShapeType::Capsule) {
        pe.dragFloat("Radius",
                     [](Node& n) { return static_cast<CollisionShapeNode&>(n).radius; },
                     [rebuild](Node& n, float v) { static_cast<CollisionShapeNode&>(n).radius = v; rebuild(n); }, 0.02f, 0.02f, 100.0f);
        pe.dragFloat("Height",
                     [](Node& n) { return static_cast<CollisionShapeNode&>(n).height; },
                     [rebuild](Node& n, float v) { static_cast<CollisionShapeNode&>(n).height = v; rebuild(n); }, 0.02f, 0.04f, 100.0f);
        const char* axes[] = {"X", "Y", "Z"};
        pe.combo("Axis",
                 [](Node& n) { return static_cast<CollisionShapeNode&>(n).axis; },
                 [rebuild](Node& n, int v) { static_cast<CollisionShapeNode&>(n).axis = v; rebuild(n); }, axes, IM_ARRAYSIZE(axes));
    } else if (shape->shapeType == CollisionShapeType::ConvexHull) {
        ImGui::TextDisabled("Convex hull of the body's mesh (dynamic-capable).");
    } else if (shape->shapeType == CollisionShapeType::Mesh) {
        ImGui::TextDisabled("Exact triangle mesh — static bodies only.");
    }
    if (shape->shapeType != CollisionShapeType::ConvexHull &&
        shape->shapeType != CollisionShapeType::Mesh)
        pe.dragFloat3("Offset",
                      [](Node& n) { return static_cast<CollisionShapeNode&>(n).offset; },
                      [rebuild](Node& n, glm::vec3 v) { static_cast<CollisionShapeNode&>(n).offset = v; rebuild(n); }, 0.02f);
}

void InspectorPanel::drawNodeHeader(Node* node, EditorUI* editor) {
    ImGui::SeparatorText("Node");

    PropertyEditor pe(*editor, node);
    pe.checkbox("##NodeEnabled",
                [](Node& n) { return n.enabled(); },
                [](Node& n, bool v) { n.setEnabled(v); });
    ImGui::SameLine();
    ImGui::Text("Name: %s", node->name().c_str());
    
    const char* typeLabel = "Node";
    if (node->asLightConst()) typeLabel = "LightNode";
    else if (node->mesh()) typeLabel = "MeshNode";
    
    ImGui::Text("Type: %s", typeLabel);

    // Groups (tags) — the sanctioned way to locate a node (e.g. a follow camera's
    // "player" target). Editable here; serialized with the node.
    ImGui::Spacing();
    ImGui::SeparatorText("Groups");
    std::string groupToRemove;
    for (const std::string& g : node->groups()) {
        ImGui::PushID(g.c_str());
        if (ImGui::SmallButton("x")) groupToRemove = g;
        ImGui::SameLine();
        ImGui::TextUnformatted(g.c_str());
        ImGui::PopID();
    }
    if (!groupToRemove.empty()) { node->removeFromGroup(groupToRemove); editor->markDirty(); }

    static char groupBuf[64] = "";
    ImGui::SetNextItemWidth(-50.0f);
    bool submit = ImGui::InputTextWithHint("##newgroup", "group name", groupBuf,
                                           sizeof(groupBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    submit |= ImGui::Button("Add");
    if (submit && groupBuf[0] != '\0') {
        node->addToGroup(groupBuf);
        editor->markDirty();
        groupBuf[0] = '\0';
    }

    if (!node->importedFromPath().empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Source: %s", node->importedFromPath().c_str());
        if (ImGui::Button("Open in 3D Importer", ImVec2(-FLT_MIN, 30))) {
            if (editor->ctxResources_) {
                editor->openModelImporter(node->importedFromPath(), editor->ctxResources_);
            }
        }
    }
}

void InspectorPanel::drawTransform(Node* node, EditorUI* editor) {
    ImGui::SeparatorText("Transform");
    PropertyEditor pe(*editor, node);

    pe.dragFloat3("Position",
                  [](Node& n) { return n.transform().position; },
                  [](Node& n, glm::vec3 v) { n.transform().position = v; }, 0.05f);

    // Rotation is edited as Euler degrees; the command stores the Euler triple
    // and reconstructs the quaternion on apply/undo.
    pe.dragFloat3("Rotation",
                  [](Node& n) { return glm::degrees(glm::eulerAngles(n.transform().rotation)); },
                  [](Node& n, glm::vec3 e) { n.transform().rotation = glm::quat(glm::radians(e)); }, 0.5f);

    pe.dragFloat3("Scale",
                  [](Node& n) { return n.transform().scale; },
                  [](Node& n, glm::vec3 v) { n.transform().scale = v; }, 0.01f, 0.001f, 100.0f);
}

void InspectorPanel::drawUINode(UINode* ui, EditorUI* editor) {
    ImGui::SeparatorText("UI Transform");

    PropertyEditor pe(*editor, ui);
    pe.dragFloat("X",
                 [](Node& n) { return static_cast<UINode&>(n).x(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setPosition(v, u.y()); }, 1.0f);
    pe.dragFloat("Y",
                 [](Node& n) { return static_cast<UINode&>(n).y(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setPosition(u.x(), v); }, 1.0f);
    pe.dragFloat("Width",
                 [](Node& n) { return static_cast<UINode&>(n).width(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setSize(v, u.height()); }, 1.0f, 0.0f);
    pe.dragFloat("Height",
                 [](Node& n) { return static_cast<UINode&>(n).height(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setSize(u.width(), v); }, 1.0f, 0.0f);
    pe.dragFloat("Pivot X",
                 [](Node& n) { return static_cast<UINode&>(n).pivotX(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setPivot(v, u.pivotY()); }, 0.01f, 0.0f, 1.0f);
    pe.dragFloat("Pivot Y",
                 [](Node& n) { return static_cast<UINode&>(n).pivotY(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setPivot(u.pivotX(), v); }, 0.01f, 0.0f, 1.0f);
    pe.dragFloat("Anchor X",
                 [](Node& n) { return static_cast<UINode&>(n).anchorX(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setAnchor(v, u.anchorY()); }, 0.01f, 0.0f, 1.0f);
    pe.dragFloat("Anchor Y",
                 [](Node& n) { return static_cast<UINode&>(n).anchorY(); },
                 [](Node& n, float v) { auto& u = static_cast<UINode&>(n); u.setAnchor(u.anchorX(), v); }, 0.01f, 0.0f, 1.0f);
}

void InspectorPanel::drawMeshRenderer(MeshNode* meshNode, EditorUI* editor) {
    ImGui::SeparatorText("Mesh Renderer");

    PropertyEditor pe(*editor, meshNode);
    pe.checkbox("Enabled##MeshEnabled",
                [](Node& n) { return static_cast<MeshNode&>(n).meshEnabled(); },
                [](Node& n, bool v) { static_cast<MeshNode&>(n).setMeshEnabled(v); });

    ResourceManager* res = editor->ctxResources_;
    std::string meshName = "Mesh [" + getAssetName(meshNode->mesh() ? res->meshId(meshNode->mesh()) : kAssetInvalid, editor) + "]";
    if (!meshNode->mesh()) meshName = "Drop Mesh Here";
    ImGui::Button(meshName.c_str(), ImVec2(-FLT_MIN, 30));
    if (res && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
            AssetID id = *(AssetID*)payload->Data;
            pe.push<AssetID>("Assign Mesh",
                             [res](Node& n) { auto* m = static_cast<MeshNode&>(n).mesh(); return m ? res->meshId(m) : kAssetInvalid; },
                             [res](Node& n, AssetID a) { if (Mesh* m = res->getMesh(a)) static_cast<MeshNode&>(n).setMesh(m); },
                             id);
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::Spacing();
    pe.checkbox("Cast Shadows",
                [](Node& n) { return static_cast<MeshNode&>(n).castShadows(); },
                [](Node& n, bool v) { static_cast<MeshNode&>(n).castShadows() = v; });
    pe.checkbox("Outline",
                [](Node& n) { return static_cast<MeshNode&>(n).outlineEnabled(); },
                [](Node& n, bool v) { static_cast<MeshNode&>(n).setOutlineEnabled(v); });
    if (meshNode->outlineEnabled()) {
        pe.colorEdit4("Outline Color",
                      [](Node& n) { return static_cast<MeshNode&>(n).outlineColor(); },
                      [](Node& n, glm::vec4 v) { static_cast<MeshNode&>(n).outlineColor() = v; });
        pe.sliderFloat("Outline Width",
                       [](Node& n) { return static_cast<MeshNode&>(n).outlineWidth(); },
                       [](Node& n, float v) { static_cast<MeshNode&>(n).outlineWidth() = v; },
                       0.0f, 12.0f);
    }

    if (!meshNode->getBehaviour<LODGroupBehaviour>()) {
        if (ImGui::Button("Add LOD Group", ImVec2(-FLT_MIN, 0.0f))) {
            meshNode->addBehaviour<LODGroupBehaviour>();
            if (meshNode->lods().empty()) {
                MeshLodLevel base;
                base.mesh = meshNode->mesh();
                base.material = meshNode->material();
                base.minScreenCoverage = 0.0f;
                meshNode->setLods({base});
            }
            editor->markDirty();
        }
    }
}

void InspectorPanel::drawLodGroup(MeshNode* meshNode, EditorUI* editor) {
    if (!meshNode || !editor || !editor->ctxResources_) return;

    if (meshNode->lods().empty()) {
        MeshLodLevel base;
        base.mesh = meshNode->mesh();
        base.material = meshNode->material();
        base.minScreenCoverage = 0.0f;
        meshNode->setLods({base});
    }

    auto& lods = meshNode->lods();
    ImGui::Text("Active LOD: %d", meshNode->activeLodIndex());
    ImGui::Text("Levels: %zu", lods.size());

    // LOD list edits are collection mutations; they mark dirty but are not undoable yet.
    if (ImGui::Button("Add LOD", ImVec2(120, 0))) {
        MeshLodLevel lvl;
        lvl.mesh = meshNode->mesh();
        lvl.material = meshNode->material();
        lvl.minScreenCoverage = lods.empty() ? 0.0f : lods.back().minScreenCoverage * 0.5f;
        lods.push_back(lvl);
        editor->markDirty();
    }
    ImGui::SameLine();
    if (ImGui::Button("Normalize Thresholds", ImVec2(170, 0))) {
        const size_t count = lods.size();
        const std::vector<float> thresholds = coverageThresholdsFromMsft({}, count);
        for (size_t i = 0; i < count; ++i)
            lods[i].minScreenCoverage = thresholds[i];
        editor->markDirty();
    }

    if (ImGui::BeginTable("LodGroupTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("LOD", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Mesh");
        ImGui::TableSetupColumn("Material");
        ImGui::TableSetupColumn("Tris", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("Min Coverage", ImGuiTableColumnFlags_WidthFixed, 120.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableHeadersRow();

        int removeIndex = -1;
        for (int i = 0; i < static_cast<int>(lods.size()); ++i) {
            ImGui::PushID(i);
            MeshLodLevel& lvl = lods[static_cast<size_t>(i)];
            Mesh* lodMesh = meshNode->meshForLod(i);
            Material* lodMaterial = meshNode->materialForLod(i);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (i == meshNode->activeLodIndex())
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), "LOD %d", i);
            else
                ImGui::Text("LOD %d", i);

            ImGui::TableSetColumnIndex(1);
            AssetID meshId = lodMesh ? editor->ctxResources_->meshId(lodMesh) : kAssetInvalid;
            std::string meshLabel = getAssetName(meshId, editor);
            if (meshLabel == "None") meshLabel = "Drop Mesh";
            ImGui::Button(meshLabel.c_str(), ImVec2(-FLT_MIN, 0));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
                    AssetID id = *(AssetID*)payload->Data;
                    if (Mesh* newMesh = editor->ctxResources_->getMesh(id)) {
                        if (i == 0) meshNode->setMesh(newMesh);
                        lvl.mesh = newMesh;
                        editor->markDirty();
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::TableSetColumnIndex(2);
            AssetID materialAlbedo = lodMaterial ? lodMaterial->desc().albedoId : kAssetInvalid;
            std::string matLabel = getAssetName(materialAlbedo, editor);
            if (matLabel == "None") matLabel = "Drop Texture";
            ImGui::Button(matLabel.c_str(), ImVec2(-FLT_MIN, 0));
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
                    AssetID id = *(AssetID*)payload->Data;
                    MaterialDesc desc = lodMaterial ? lodMaterial->desc() : MaterialDesc{};
                    desc.albedoId = id;
                    if (Material* mat = editor->ctxResources_->getMaterial(desc)) {
                        if (i == 0) meshNode->setMaterial(mat);
                        lvl.material = mat;
                        editor->markDirty();
                    }
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::TableSetColumnIndex(3);
            if (lodMesh)
                ImGui::Text("%u", lodMesh->allocation().indexCount / 3);
            else
                ImGui::TextDisabled("-");

            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##coverage", &lvl.minScreenCoverage, 0.01f, 0.0f, 1.0f, "%.3f"))
                editor->markDirty();

            ImGui::TableSetColumnIndex(5);
            if (i > 0 && ImGui::Button("X"))
                removeIndex = i;

            ImGui::PopID();
        }

        if (removeIndex > 0) {
            lods.erase(lods.begin() + removeIndex);
            editor->markDirty();
        }

        ImGui::EndTable();
    }
}

void InspectorPanel::drawMaterial(Material* material, MeshNode* meshNode, EditorUI* editor) {
    ImGui::SeparatorText("Material");

    ResourceManager* res = editor->ctxResources_;
    PropertyEditor pe(*editor, meshNode);

    // Material edits rebuild the interned MaterialDesc so undo swaps descriptors.
    auto MG = [](auto m) {
        return [m](Node& n) {
            Material* mat = static_cast<MeshNode&>(n).material();
            return (mat ? mat->desc() : MaterialDesc{}).*m;
        };
    };
    auto MS = [res](auto m) {
        return [res, m](Node& n, auto v) {
            auto& mn = static_cast<MeshNode&>(n);
            MaterialDesc d = mn.material() ? mn.material()->desc() : MaterialDesc{};
            d.*m = v;
            if (Material* nm = res->getMaterial(d)) mn.setMaterial(nm);
        };
    };

    std::string texName = "Base Texture [" + getAssetName(material ? material->desc().albedoId : kAssetInvalid, editor) + "]";
    if (!material || material->desc().albedoId == kAssetInvalid) texName = "Drop Base Texture Here";
    ImGui::Button(texName.c_str(), ImVec2(-FLT_MIN, 30));
    if (res && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_ID")) {
            AssetID id = *(AssetID*)payload->Data;
            pe.push<AssetID>("Base Texture", MG(&MaterialDesc::albedoId), MS(&MaterialDesc::albedoId), id);
        }
        ImGui::EndDragDropTarget();
    }

    // MS starts from a default desc, so meshes with no material become editable.
    if (res) {
        // Shading model (Unity Lit / Unlit). Switching re-interns the material with
        // the new MaterialType; the renderer picks the matching pipeline per draw.
        static const char* const kShaderItems[] = {"Lit (PBR)", "Unlit"};
        pe.combo("Shader",
            [](Node& n) {
                Material* mat = static_cast<MeshNode&>(n).material();
                return static_cast<int>(mat ? mat->desc().type : MaterialType::Lit);
            },
            [res](Node& n, int v) {
                auto& mn = static_cast<MeshNode&>(n);
                MaterialDesc d = mn.material() ? mn.material()->desc() : MaterialDesc{};
                d.type = static_cast<MaterialType>(v);
                if (Material* nm = res->getMaterial(d)) mn.setMaterial(nm);
            },
            kShaderItems, 2);

        const bool unlit = material && material->desc().type == MaterialType::Unlit;
        pe.colorEdit4("Base Color", MG(&MaterialDesc::baseColor), MS(&MaterialDesc::baseColor));
        pe.colorEdit4("Emissive", MG(&MaterialDesc::emissiveColor), MS(&MaterialDesc::emissiveColor));
        // Lighting-only parameters — irrelevant to the unlit model, so hidden there.
        if (!unlit) {
            pe.sliderFloat("Metallic", MG(&MaterialDesc::metallic), MS(&MaterialDesc::metallic), 0.0f, 1.0f);
            pe.sliderFloat("Roughness", MG(&MaterialDesc::roughness), MS(&MaterialDesc::roughness), 0.0f, 1.0f);
            pe.sliderFloat("Ambient Occlusion", MG(&MaterialDesc::ao), MS(&MaterialDesc::ao), 0.0f, 1.0f);
        }
    }
}

void InspectorPanel::drawBehaviours(Node* node, EditorUI* editor) {
    ImGui::SeparatorText("Behaviours");
    std::string removeType;
    size_t removeIndex = 0;

    for (const auto& b : node->behaviours()) {
        if (!b->visibleInEditor()) continue;
        ImGui::PushID(b.get());

        const char* typeName = b->typeName() ? b->typeName() : "Unknown Behaviour";
        const size_t typeIndex = behaviourTypeIndex(node, b.get());

        bool enabled = b->enabled();
        if (ImGui::Checkbox("##enabled", &enabled)) {
            const bool oldEnabled = !enabled;
            const std::string type = typeName;
            editor->execute(std::make_unique<SetPropertyCommand>(
                node->id(), "Set Behaviour Enabled",
                [type, typeIndex, oldEnabled](Node& n) {
                    applyBehaviourEnabled(n, type, typeIndex, oldEnabled);
                },
                [type, typeIndex, enabled](Node& n) {
                    applyBehaviourEnabled(n, type, typeIndex, enabled);
                }));
        }
        ImGui::SameLine();

        bool expanded = ImGui::CollapsingHeader(typeName, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

        ImGui::SameLine(ImGui::GetWindowWidth() - 30.0f);
        if (ImGui::Button("X")) {
            if (b->typeName()) {
                removeType = b->typeName();
                removeIndex = typeIndex;
            }
        }

        if (expanded) {
            if (dynamic_cast<LODGroupBehaviour*>(b.get())) {
                if (auto* meshNode = dynamic_cast<MeshNode*>(node))
                    drawLodGroup(meshNode, editor);
                else
                    ImGui::TextDisabled("LOD Group requires a MeshNode.");
            } else {
                bool drew = false;
                if (b->typeName()) {
                    if (const reflect::TypeDesc* td = reflect::TypeRegistry::instance().find(b->typeName())) {
                        if (auto command = drawReflectedBehaviourProperties(*td, node, b.get()))
                            editor->execute(std::move(command));
                        drew = !td->properties.empty();
                    }
                }
                if (!drew) {
                    drew = InspectorRegistry::instance().draw(*b, *editor);
                }
                if (!drew) {
                    ImGui::TextDisabled("No editable properties.");
                }
            }

            // Animator: drop a .bvh from the file browser to add it as a clip.
            if (auto* anim = dynamic_cast<Animator*>(b.get())) {
                ImGui::Button("Drop .bvh to add clip", ImVec2(-FLT_MIN, 24));
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_BVH")) {
                        if (ResourceManager* res = editor->ctxResources_) {
                            AssetID id = BVHLoader::load((const char*)payload->Data, *res);
                            if (id != kAssetInvalid)
                                if (AnimationClip* clip = res->getAnimation(id))
                                    anim->addClip(clip->name(), clip);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
            }
        }
        ImGui::PopID();
    }

    if (!removeType.empty()) {
        editor->execute(std::make_unique<RemoveBehaviourCommand>(
            node->id(), removeType, removeIndex));
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Add Component", ImVec2(-1.0f, 0.0f))) {
        ImGui::OpenPopup("AddComponentPopup");
    }

    if (ImGui::BeginPopup("AddComponentPopup")) {
        auto& factories = BehaviourRegistry::instance().factories();
        for (const auto& pair : factories) {
            if (ImGui::Selectable(pair.first.c_str())) {
                editor->execute(std::make_unique<AddBehaviourCommand>(
                    node->id(), pair.first));
            }
        }
        ImGui::EndPopup();
    }
}

} // namespace saida
