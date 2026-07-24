#include "editor/panels/SceneHierarchyPanel.hpp"
#include "editor/EditorUI.hpp"
#include "scene/Scene.hpp"
#include "scene/Node.hpp"
#include "nodes/MeshNode.hpp"
#include "nodes/LightNode.hpp"
#include "nodes/CameraNode.hpp"
#include "nodes/WaterNode.hpp"
#include "nodes/ParticleSystemNode.hpp"
#include "physics/StaticBodyNode.hpp"
#include "physics/RigidBodyNode.hpp"
#include "physics/AreaNode.hpp"
#include "physics/CharacterBodyNode.hpp"
#include "physics/CollisionShapeNode.hpp"
#include "physics/JointNodes.hpp"
#include "scene/SceneSerializer.hpp"
#include "editor/Command.hpp"
#include "project/Project.hpp"
#include "graphics/ResourceManager.hpp"
#include "nodes/UICanvasNode.hpp"
#include "nodes/UIColorNode.hpp"
#include "nodes/UIImageNode.hpp"
#include "scene/GLTFLoader.hpp"
#include "nodes/UITextNode.hpp"
#include "nodes/UIButtonNode.hpp"
#include "nodes/UIToggleNode.hpp"
#include "nodes/WebCanvasNode.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <filesystem>
#include <cstring>
#include <cctype>
#include <algorithm>

namespace saida {

namespace {

static std::string toLower(const std::string& str) {
    std::string lower = str;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
    return lower;
}

// Shared factory for "Create Child" and "Create Parent".
static std::unique_ptr<Node> makeNodeOfType(CreateNodeType type, Mesh* defaultMesh,
                                            Material* defaultMaterial, ResourceManager* resources,
                                            bool bodyWithCube) {
    // Blank scenes need defaults so the first mesh is visible and serializable.
    if (resources) {
        if (!defaultMesh) defaultMesh = resources->getMesh(kAssetBuiltinCube);
        if (!defaultMaterial) defaultMaterial = resources->getMaterial(MaterialDesc{});
    }
    switch (type) {
        case CreateNodeType::Node:
            return std::make_unique<Node>("Node");
        case CreateNodeType::MeshNode:
            return std::make_unique<MeshNode>("MeshNode", defaultMesh, defaultMaterial);
        case CreateNodeType::DirectionalLight:
            return std::make_unique<LightNode>("Directional Light", LightType::Directional);
        case CreateNodeType::PointLight:
            return std::make_unique<LightNode>("Point Light", LightType::Point);
        case CreateNodeType::SpotLight:
            return std::make_unique<LightNode>("Spot Light", LightType::Spot);
        case CreateNodeType::Camera:
            return std::make_unique<CameraNode>("Camera");
        case CreateNodeType::Water:
            return std::make_unique<WaterNode>();
        case CreateNodeType::ParticleSystem:
            return std::make_unique<ParticleSystemNode>();
        case CreateNodeType::UICanvas: {
            auto n = std::make_unique<UICanvasNode>(); n->setName("UICanvas"); return n;
        }
        case CreateNodeType::UIColorNode: {
            auto n = std::make_unique<UIColorNode>(); n->setName("UIColor"); return n;
        }
        case CreateNodeType::UIImageNode: {
            auto n = std::make_unique<UIImageNode>(); n->setName("UIImage"); return n;
        }
        case CreateNodeType::UITextNode: {
            auto n = std::make_unique<UITextNode>(); n->setName("UIText"); return n;
        }
        case CreateNodeType::UIButtonNode: {
            auto n = std::make_unique<UIButtonNode>(); n->setName("UIButton"); return n;
        }
        case CreateNodeType::UIToggleNode: {
            auto n = std::make_unique<UIToggleNode>(); n->setName("UIToggle"); return n;
        }
        case CreateNodeType::UIExample: {
            auto canvas = std::make_unique<UICanvasNode>();
            canvas->setName("UICanvas");
            auto title = std::make_unique<UITextNode>();
            title->setName("TitleText"); title->setText("UI Example");
            title->setPosition(200.0f, 50.0f); title->setFontSize(32.0f);
            auto button = std::make_unique<UIButtonNode>();
            button->setName("Button"); button->setPosition(200.0f, 150.0f);
            button->setSize(200.0f, 60.0f);
            auto btnText = std::make_unique<UITextNode>();
            btnText->setName("ButtonText"); btnText->setText("Click Me!");
            btnText->setPosition(100.0f, 30.0f); btnText->setPivot(0.5f, 0.5f);
            auto icon = std::make_unique<UIImageNode>();
            icon->setName("Icon"); icon->setPosition(20.0f, 30.0f);
            icon->setSize(40.0f, 40.0f); icon->setPivot(0.5f, 0.5f);
            button->addChild(std::move(icon));
            button->addChild(std::move(btnText));
            canvas->addChild(std::move(title));
            canvas->addChild(std::move(button));
            return canvas;
        }
        case CreateNodeType::WebCanvas: {
            auto web = std::make_unique<WebCanvasNode>();
            web->setName("WebCanvas");
            if (resources)
                web->init(resources->device(), 1920, 1080, WebCanvasNode::Mode::ScreenSpace);
            return web;
        }
        case CreateNodeType::StaticBody:
        case CreateNodeType::RigidBody:
        case CreateNodeType::CharacterBody:
        case CreateNodeType::Area: {
            std::unique_ptr<CollisionObjectNode> body;
            const char* label = "Body";
            if (type == CreateNodeType::StaticBody) { body = std::make_unique<StaticBodyNode>(); label = "StaticBody"; }
            else if (type == CreateNodeType::RigidBody) { body = std::make_unique<RigidBodyNode>(); label = "RigidBody"; }
            else if (type == CreateNodeType::CharacterBody) { body = std::make_unique<CharacterBodyNode>(); label = "CharacterBody"; }
            else { body = std::make_unique<AreaNode>(); label = "Area"; }
            body->setName(label);
            if (bodyWithCube)
                body->addChild(std::make_unique<MeshNode>("Mesh", defaultMesh, defaultMaterial));
            body->addChild(std::make_unique<CollisionShapeNode>());
            return body;
        }
        case CreateNodeType::CollisionShape:
            return std::make_unique<CollisionShapeNode>();
        case CreateNodeType::FixedJoint:
            return std::make_unique<FixedJointNode>();
        case CreateNodeType::PointJoint:
            return std::make_unique<PointJointNode>();
        case CreateNodeType::HingeJoint:
            return std::make_unique<HingeJointNode>();
        default:
            return nullptr;  // None / SceneInstance: handled by the caller
    }
}

static bool nodeMatchesSearch(Node* node, const std::string& query) {
    if (query.empty()) return true;
    
    std::string nameLower = toLower(node->name());
    
    std::string typeLower = "node";
    if (dynamic_cast<MeshNode*>(node)) typeLower = "meshnode";
    else if (dynamic_cast<LightNode*>(node)) typeLower = "lightnode";
    else if (dynamic_cast<ParticleSystemNode*>(node)) typeLower = "particlesystem";
    else if (dynamic_cast<Scene*>(node)) typeLower = "scene";

    if (nameLower.find(query) != std::string::npos || typeLower.find(query) != std::string::npos) {
        return true;
    }
    
    for (auto& child : node->children()) {
        if (nodeMatchesSearch(child.get(), query)) return true;
    }
    return false;
}

const char* nodeTypeIcon(const Node* node) {
    if (node->asLightConst())    return "[L]";
    if (node->mesh())            return "[M]";
    if (dynamic_cast<const ParticleSystemNode*>(node)) return "[FX]";
    return "[N]";
}

bool isDescendantOf(const Node* potentialDescendant, const Node* potentialAncestor) {
    const Node* curr = potentialDescendant;
    while (curr) {
        if (curr == potentialAncestor) {
            return true;
        }
        curr = curr->parent();
    }
    return false;
}

size_t childIndex(const Node& parent, const Node* child) {
    const auto& children = parent.children();
    for (size_t i = 0; i < children.size(); ++i) {
        if (children[i].get() == child) return i;
    }
    return children.size();
}

bool canInsertNode(Node* draggedNode, Node* parent, size_t index) {
    if (!draggedNode || !draggedNode->parent() || !parent) return false;
    if (draggedNode == parent || isDescendantOf(parent, draggedNode)) return false;

    if (draggedNode->parent() == parent) {
        const size_t oldIndex = childIndex(*parent, draggedNode);
        if (index == oldIndex || index == oldIndex + 1) return false;
    }
    return true;
}

void drawInsertLine(const ImRect& rect) {
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImU32 color = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    const float y = rect.GetCenter().y;
    drawList->AddLine(ImVec2(rect.Min.x, y), ImVec2(rect.Max.x, y), color, 2.0f);
    drawList->AddCircleFilled(ImVec2(rect.Min.x, y), 3.0f, color);
    drawList->AddCircleFilled(ImVec2(rect.Max.x, y), 3.0f, color);
}

bool drawInsertionDropTarget(Node* parent, size_t index, Node*& outDraggedNode) {
    outDraggedNode = nullptr;

    const ImGuiPayload* activePayload = ImGui::GetDragDropPayload();
    if (!activePayload || !activePayload->IsDataType("SCENE_NODE")) return false;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (!window || window->SkipItems) return false;

    constexpr float kDropZoneHalfHeight = 4.0f;
    const ImVec2 cursor = ImGui::GetCursorScreenPos();
    const float width = std::max(16.0f, ImGui::GetContentRegionAvail().x);
    ImRect rect(ImVec2(cursor.x, cursor.y - kDropZoneHalfHeight),
                ImVec2(cursor.x + width, cursor.y + kDropZoneHalfHeight));

    ImGui::PushID(parent);
    ImGui::PushID(static_cast<int>(index));
    const ImGuiID id = ImGui::GetID("node_insert_drop");

    bool drawFeedback = false;
    bool delivered = false;
    if (ImGui::BeginDragDropTargetCustom(rect, id)) {
        const ImGuiDragDropFlags flags = ImGuiDragDropFlags_AcceptBeforeDelivery |
                                         ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_NODE", flags)) {
            Node* draggedNode = *(Node**)payload->Data;
            if (canInsertNode(draggedNode, parent, index)) {
                drawFeedback = true;
                if (payload->IsDelivery()) {
                    outDraggedNode = draggedNode;
                    delivered = true;
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (drawFeedback) drawInsertLine(rect);
    ImGui::PopID();
    ImGui::PopID();
    return delivered;
}

} // namespace

void SceneHierarchyPanel::draw(EditorUI* editor, Scene* scene) {
    ImGui::Begin("Scene Tree", &editor->showSceneTree_);

    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##SceneTreeSearch", "Search by name or type...", editor->sceneTreeSearchBuf_, sizeof(editor->sceneTreeSearchBuf_));
    ImGui::PopItemWidth();
    ImGui::Separator();

    if (scene) {
        drawSceneTreeNode(editor, scene);
    } else {
        ImGui::TextDisabled("(no scene)");
    }

    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::IsAnyItemHovered()) {
        editor->selectedNode_ = nullptr;
    }

    if (ImGui::BeginPopupContextWindow("SceneTreeContext", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
        if (scene) {
            if (ImGui::BeginMenu("Create Node")) {
                if (ImGui::MenuItem("Node")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::Node;
                }
                if (ImGui::MenuItem("MeshNode (Cube)")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::MeshNode;
                }
                if (ImGui::MenuItem("Directional Light")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::DirectionalLight;
                }
                if (ImGui::MenuItem("Point Light")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::PointLight;
                }
                if (ImGui::MenuItem("Spot Light")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::SpotLight;
                }
                if (ImGui::MenuItem("Camera")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::Camera;
                }
                if (ImGui::MenuItem("Water")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::Water;
                }
                if (ImGui::MenuItem("Particle System")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::ParticleSystem;
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("UI Nodes")) {
                    if (ImGui::MenuItem("UI Canvas")) {
                        editor->nodeToCreateChildUnder_ = scene;
                        editor->createType_ = CreateNodeType::UICanvas;
                    }
                    if (ImGui::MenuItem("Color Node")) {
                        editor->nodeToCreateChildUnder_ = scene;
                        editor->createType_ = CreateNodeType::UIColorNode;
                    }
                    if (ImGui::MenuItem("Image Node")) {
                        editor->nodeToCreateChildUnder_ = scene;
                        editor->createType_ = CreateNodeType::UIImageNode;
                    }
                    if (ImGui::MenuItem("Text Node")) {
                        editor->nodeToCreateChildUnder_ = scene;
                        editor->createType_ = CreateNodeType::UITextNode;
                    }
                    if (ImGui::MenuItem("Button Node")) {
                        editor->nodeToCreateChildUnder_ = scene;
                        editor->createType_ = CreateNodeType::UIButtonNode;
                    }
                    if (ImGui::MenuItem("Toggle Node")) {
                        editor->nodeToCreateChildUnder_ = scene;
                        editor->createType_ = CreateNodeType::UIToggleNode;
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem("UI Example")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::UIExample;
                }
                ImGui::Separator();
                if (ImGui::BeginMenu("Physics")) {
                    // Create under the selected node when there is one (so a body
                    // wraps an existing mesh), otherwise under the scene root.
                    Node* parent = editor->selectedNode_ ? editor->selectedNode_ : scene;
                    if (ImGui::MenuItem("Static Body")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::StaticBody;
                    }
                    if (ImGui::MenuItem("Rigid Body")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::RigidBody;
                    }
                    if (ImGui::MenuItem("Character Body")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::CharacterBody;
                    }
                    if (ImGui::MenuItem("Area (Trigger)")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::Area;
                    }
                    if (ImGui::MenuItem("Collision Shape")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::CollisionShape;
                    }
                    if (ImGui::MenuItem("Fixed Joint")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::FixedJoint;
                    }
                    if (ImGui::MenuItem("Point Joint")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::PointJoint;
                    }
                    if (ImGui::MenuItem("Hinge Joint")) {
                        editor->nodeToCreateChildUnder_ = parent;
                        editor->createType_ = CreateNodeType::HingeJoint;
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Web Canvas")) {
                    editor->nodeToCreateChildUnder_ = scene;
                    editor->createType_ = CreateNodeType::WebCanvas;
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    // Play is read-only: drop any mutation queued from a context menu this frame
    // instead of applying it (and avoid its selection side effects).
    if (!editor->canEdit()) {
        editor->nodeToDelete_ = nullptr;
        editor->nodeToReparent_ = nullptr;
        editor->newParent_ = nullptr;
        editor->newChildIndex_ = static_cast<size_t>(-1);
        editor->nodeToCreateChildUnder_ = nullptr;
        editor->createType_ = CreateNodeType::None;
        editor->nodeToCreateParentFor_ = nullptr;
        editor->createParentType_ = CreateNodeType::None;
        editor->nodeToRename_ = nullptr;
    }

    if (editor->nodeToDelete_) {
        if (editor->nodeToDelete_->parent()) {
            if (editor->selectedNode_ && (editor->selectedNode_ == editor->nodeToDelete_ || isDescendantOf(editor->selectedNode_, editor->nodeToDelete_)))
                editor->selectedNode_ = nullptr;
            editor->execute(
                std::make_unique<DeleteNodeCommand>(editor->nodeToDelete_->id()));
        }
        editor->nodeToDelete_ = nullptr;
    }

    if (editor->nodeToReparent_ && editor->newParent_) {
        if (editor->nodeToReparent_->parent() && !isDescendantOf(editor->newParent_, editor->nodeToReparent_))
            editor->execute(std::make_unique<ReparentNodeCommand>(
                editor->nodeToReparent_->id(), editor->newParent_->id(), editor->newChildIndex_));
        editor->nodeToReparent_ = nullptr;
        editor->newParent_ = nullptr;
        editor->newChildIndex_ = static_cast<size_t>(-1);
    }

    if (editor->nodeToCreateChildUnder_ && editor->createType_ != CreateNodeType::None) {
        Mesh* defaultMesh = nullptr;
        Material* defaultMaterial = nullptr;
        if (scene) {
            scene->traverse([&](Node& n, const glm::mat4&) {
                if (!defaultMesh && n.mesh()) { defaultMesh = n.mesh(); defaultMaterial = n.material(); }
            });
        }

        std::unique_ptr<Node> newNode;
        if (editor->createType_ == CreateNodeType::SceneInstance && editor->ctxResources_ && editor->ctxProject_) {
            std::filesystem::path p(editor->draggedScenePath_);
            newNode = SceneSerializer::loadNodeFromSceneFile(editor->draggedScenePath_, *editor->ctxResources_);
            if (newNode) {
                newNode->setName(p.stem().string() + " (Scene)");
                if (Scene* s = dynamic_cast<Scene*>(newNode.get())) {
                    std::string relativePath = std::filesystem::path(editor->draggedScenePath_).lexically_relative(editor->ctxProject_->rootPath()).generic_string();
                    AssetID id = editor->ctxResources_->getOrRegister(relativePath, AssetType::Scene);
                    s->setPrefabAssetId(id);
                }
            }
        } else {
            newNode = makeNodeOfType(editor->createType_, defaultMesh, defaultMaterial, editor->ctxResources_, /*bodyWithCube=*/true);
        }

        if (newNode) {
            NodeId addedId = newNode->id();
            editor->execute(std::make_unique<AddNodeCommand>(
                editor->nodeToCreateChildUnder_->id(), std::move(newNode)));
            editor->selectedNode_ = editor->document_.find(addedId);
        }
        editor->nodeToCreateChildUnder_ = nullptr;
        editor->createType_ = CreateNodeType::None;
    }

    if (editor->nodeToCreateParentFor_ && editor->createParentType_ != CreateNodeType::None) {
        if (editor->nodeToCreateParentFor_->parent()) {
            // A body wrapping an existing node gets only an Auto CollisionShape
            // (no cube) — the wrapped node is the visual, and Auto measures it.
            Mesh* defaultMesh = nullptr;
            Material* defaultMaterial = nullptr;
            if (scene) {
                scene->traverse([&](Node& n, const glm::mat4&) {
                    if (!defaultMesh && n.mesh()) { defaultMesh = n.mesh(); defaultMaterial = n.material(); }
                });
            }
            auto newParent = makeNodeOfType(editor->createParentType_, defaultMesh, defaultMaterial, editor->ctxResources_, /*bodyWithCube=*/false);
            if (newParent) {
                NodeId parentId = newParent->id();
                editor->execute(std::make_unique<CreateParentCommand>(
                    editor->nodeToCreateParentFor_->id(), std::move(newParent)));
                editor->selectedNode_ = editor->document_.find(parentId);
            }
        }
        editor->nodeToCreateParentFor_ = nullptr;
        editor->createParentType_ = CreateNodeType::None;
    }

    ImGui::Dummy(ImGui::GetContentRegionAvail());
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_SCENE")) {
            editor->nodeToCreateChildUnder_ = scene;
            editor->createType_ = CreateNodeType::SceneInstance;
            editor->draggedScenePath_ = (const char*)payload->Data;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_MODEL")) {
            std::string path = (const char*)payload->Data;
            if (scene && editor->ctxResources_) {
                GLTFLoadOptions opts;
                if (editor->ctxProject()) opts.autoMeshLods = editor->ctxProject()->autoMeshLods();
                GLTFLoader::load(path, *scene, *editor->ctxResources_, opts);
            }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::End();
}

void SceneHierarchyPanel::drawSceneTreeNode(EditorUI* editor, Node* node) {
    std::string query = toLower(editor->sceneTreeSearchBuf_);
    if (!nodeMatchesSearch(node, query)) {
        return; 
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow
                             | ImGuiTreeNodeFlags_OpenOnDoubleClick
                             | ImGuiTreeNodeFlags_SpanAvailWidth
                             | ImGuiTreeNodeFlags_FramePadding;

    bool isNestedScene = (dynamic_cast<Scene*>(node) != nullptr && node != editor->ctxScene_ && static_cast<Scene*>(node)->prefabAssetId() != kAssetInvalid);
    bool isLeaf = node->children().empty() || isNestedScene;
    if (isLeaf)
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (node == editor->selectedNode_)
        flags |= ImGuiTreeNodeFlags_Selected;
    
    if (!node->parent() || !query.empty())
        flags |= ImGuiTreeNodeFlags_DefaultOpen;

    const char* icon = nodeTypeIcon(node);
    if (isNestedScene) icon = "[P]";

    bool open = false;

    bool active = node->isActiveInHierarchy();
    bool hasCustomColor = false;
    ImVec4 customColor;
    const bool lightTheme = editor->settings_.lightTheme();
    
    if (active) {
        if (isNestedScene) {
            hasCustomColor = true;
            customColor = lightTheme ? ImVec4(0.039f, 0.278f, 0.490f, 1.0f)
                                     : ImVec4(0.722f, 0.769f, 0.733f, 1.0f);
        } else if (dynamic_cast<MeshNode*>(node)) {
            hasCustomColor = true;
            customColor = lightTheme ? ImVec4(0.235f, 0.596f, 0.329f, 1.0f)
                                     : ImVec4(0.494f, 0.796f, 0.388f, 1.0f);
        } else if (dynamic_cast<LightNode*>(node)) {
            hasCustomColor = true;
            customColor = lightTheme ? ImVec4(0.776f, 0.518f, 0.129f, 1.0f)
                                     : ImVec4(0.886f, 0.690f, 0.298f, 1.0f);
        } else if (dynamic_cast<ParticleSystemNode*>(node)) {
            hasCustomColor = true;
            customColor = lightTheme ? ImVec4(0.784f, 0.294f, 0.227f, 1.0f)
                                     : ImVec4(0.898f, 0.420f, 0.353f, 1.0f);
        }
        if (lightTheme && hasCustomColor)
            customColor = ImVec4(0.106f, 0.310f, 0.125f, 1.0f);
    }

    if (!active) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    } else if (hasCustomColor) {
        ImGui::PushStyleColor(ImGuiCol_Text, customColor);
    }

    if (editor->nodeToRename_ == node) {
        open = ImGui::TreeNodeEx(reinterpret_cast<void*>(node), flags, "%s", icon);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename_node", editor->nodeRenameBuf_, sizeof(editor->nodeRenameBuf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (node->name() != editor->nodeRenameBuf_)
                editor->execute(std::make_unique<RenameNodeCommand>(
                    node->id(), node->name(), editor->nodeRenameBuf_));
            editor->nodeToRename_ = nullptr;
        } else if (ImGui::IsItemDeactivated() && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
            if (node->name() != editor->nodeRenameBuf_)
                editor->execute(std::make_unique<RenameNodeCommand>(
                    node->id(), node->name(), editor->nodeRenameBuf_));
            editor->nodeToRename_ = nullptr;
        }
        ImGui::PopStyleVar();
    } else {
        open = ImGui::TreeNodeEx(reinterpret_cast<void*>(node), flags, "%s %s", icon, node->name().c_str());
    }

    if (!active || hasCustomColor) {
        ImGui::PopStyleColor();
    }

    if (ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("SCENE_NODE", &node, sizeof(Node*));
        ImGui::Text("Move %s", node->name().c_str());
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SCENE_NODE")) {
            Node* draggedNode = *(Node**)payload->Data;
            if (draggedNode != node && draggedNode->parent() != node && !isDescendantOf(node, draggedNode)) {
                editor->nodeToReparent_ = draggedNode;
                editor->newParent_ = node;
                editor->newChildIndex_ = static_cast<size_t>(-1);
            }
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_SCENE")) {
            editor->nodeToCreateChildUnder_ = node;
            editor->createType_ = CreateNodeType::SceneInstance;
            editor->draggedScenePath_ = (const char*)payload->Data;
        }
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("FILE_MODEL")) {
            std::string path = (const char*)payload->Data;
            if (editor->ctxResources_) {
                GLTFLoadOptions opts;
                if (editor->ctxProject()) opts.autoMeshLods = editor->ctxProject()->autoMeshLods();
                GLTFLoader::load(path, *node, *editor->ctxResources_, opts);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        editor->selectedNode_ = node;

    if (isNestedScene && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && editor->ctxResources_ && editor->ctxProject_) {
        std::string absPath = editor->ctxResources_->getRegistry()->getAbsolutePath(static_cast<Scene*>(node)->prefabAssetId());
        if (!absPath.empty()) {
            editor->loadScene(absPath);
        }
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Middle))
        node->setEnabled(!node->enabled());

    if (ImGui::BeginPopupContextItem()) {
        editor->selectedNode_ = node; 

        if (ImGui::MenuItem("Rename")) {
            editor->nodeToRename_ = node;
            std::strncpy(editor->nodeRenameBuf_, node->name().c_str(), sizeof(editor->nodeRenameBuf_) - 1);
            editor->nodeRenameBuf_[sizeof(editor->nodeRenameBuf_) - 1] = '\0';
        }
        if (node->enabled()) {
            if (ImGui::MenuItem("Hide")) {
                node->setEnabled(false);
            }
        } else {
            if (ImGui::MenuItem("Show")) {
                node->setEnabled(true);
            }
        }
        ImGui::Separator();

        if (ImGui::BeginMenu("Create Child")) {
            if (ImGui::MenuItem("Node")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::Node;
            }
            if (ImGui::MenuItem("MeshNode (Cube)")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::MeshNode;
            }
            if (ImGui::MenuItem("Directional Light")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::DirectionalLight;
            }
            if (ImGui::MenuItem("Point Light")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::PointLight;
            }
            if (ImGui::MenuItem("Spot Light")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::SpotLight;
            }
            if (ImGui::MenuItem("Camera")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::Camera;
            }
            if (ImGui::MenuItem("Water")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::Water;
            }
            if (ImGui::MenuItem("Particle System")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::ParticleSystem;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("UI Nodes")) {
                if (ImGui::MenuItem("UI Canvas")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::UICanvas;
                }
                if (ImGui::MenuItem("Color Node")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::UIColorNode;
                }
                if (ImGui::MenuItem("Image Node")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::UIImageNode;
                }
                if (ImGui::MenuItem("Text Node")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::UITextNode;
                }
                if (ImGui::MenuItem("Button Node")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::UIButtonNode;
                }
                if (ImGui::MenuItem("Toggle Node")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::UIToggleNode;
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("UI Example")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::UIExample;
            }
            ImGui::Separator();
            if (ImGui::BeginMenu("Physics")) {
                if (ImGui::MenuItem("Static Body")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::StaticBody;
                }
                if (ImGui::MenuItem("Rigid Body")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::RigidBody;
                }
                if (ImGui::MenuItem("Character Body")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::CharacterBody;
                }
                if (ImGui::MenuItem("Area (Trigger)")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::Area;
                }
                if (ImGui::MenuItem("Collision Shape")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::CollisionShape;
                }
                if (ImGui::MenuItem("Fixed Joint")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::FixedJoint;
                }
                if (ImGui::MenuItem("Point Joint")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::PointJoint;
                }
                if (ImGui::MenuItem("Hinge Joint")) {
                    editor->nodeToCreateChildUnder_ = node;
                    editor->createType_ = CreateNodeType::HingeJoint;
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Web Canvas")) {
                editor->nodeToCreateChildUnder_ = node;
                editor->createType_ = CreateNodeType::WebCanvas;
            }
            ImGui::EndMenu();
        }

        bool canCreateParent = node->parent() != nullptr;
        if (canCreateParent && ImGui::BeginMenu("Create Parent")) {
            // Same type list as Create Child; wraps this node under the new parent.
            auto parentItem = [&](const char* lbl, CreateNodeType t) {
                if (ImGui::MenuItem(lbl)) {
                    editor->nodeToCreateParentFor_ = node;
                    editor->createParentType_ = t;
                }
            };
            parentItem("Node", CreateNodeType::Node);
            parentItem("MeshNode (Cube)", CreateNodeType::MeshNode);
            parentItem("Directional Light", CreateNodeType::DirectionalLight);
            parentItem("Point Light", CreateNodeType::PointLight);
            parentItem("Spot Light", CreateNodeType::SpotLight);
            ImGui::Separator();
            if (ImGui::BeginMenu("Physics##parent")) {
                parentItem("Static Body", CreateNodeType::StaticBody);
                parentItem("Rigid Body", CreateNodeType::RigidBody);
                parentItem("Character Body", CreateNodeType::CharacterBody);
                parentItem("Area (Trigger)", CreateNodeType::Area);
                parentItem("Fixed Joint", CreateNodeType::FixedJoint);
                parentItem("Point Joint", CreateNodeType::PointJoint);
                parentItem("Hinge Joint", CreateNodeType::HingeJoint);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("UI Nodes##parent")) {
                parentItem("UI Canvas", CreateNodeType::UICanvas);
                parentItem("Color Node", CreateNodeType::UIColorNode);
                parentItem("Image Node", CreateNodeType::UIImageNode);
                parentItem("Text Node", CreateNodeType::UITextNode);
                parentItem("Button Node", CreateNodeType::UIButtonNode);
                parentItem("Toggle Node", CreateNodeType::UIToggleNode);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            editor->duplicateSelected();
        }
        
        bool canDelete = node->parent() != nullptr;
        if (ImGui::MenuItem("Delete", "Del", false, canDelete)) {
            editor->nodeToDelete_ = node;
        }

        ImGui::EndPopup();
    }

    if (open) {
        if (!isLeaf) {
            const bool allowInsertionTargets = query.empty();
            size_t index = 0;
            for (auto& child : node->children()) {
                Node* draggedNode = nullptr;
                if (allowInsertionTargets && drawInsertionDropTarget(node, index, draggedNode)) {
                    editor->nodeToReparent_ = draggedNode;
                    editor->newParent_ = node;
                    editor->newChildIndex_ = index;
                }
                drawSceneTreeNode(editor, child.get());
                ++index;
            }
            Node* draggedNode = nullptr;
            if (allowInsertionTargets && drawInsertionDropTarget(node, index, draggedNode)) {
                editor->nodeToReparent_ = draggedNode;
                editor->newParent_ = node;
                editor->newChildIndex_ = index;
            }
            ImGui::TreePop();
        }
    }
}

} // namespace saida
