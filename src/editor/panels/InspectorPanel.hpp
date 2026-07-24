#pragma once

namespace saida {

class Node;
class MeshNode;
class Material;
class EditorUI;
class CollisionObjectNode;
class CollisionShapeNode;

class InspectorPanel {
public:
    void draw(EditorUI* editor);

private:
    void drawNodeHeader(Node* node, EditorUI* editor);
    void drawTransform(Node* node, EditorUI* editor);
    void drawUINode(class UINode* node, EditorUI* editor);
    void drawMeshRenderer(class MeshNode* meshNode, EditorUI* editor);
    void drawLodGroup(class MeshNode* meshNode, EditorUI* editor);
    void drawMaterial(Material* material, MeshNode* meshNode, EditorUI* editor);
    void drawPhysicsBody(CollisionObjectNode* body, EditorUI* editor);
    void drawCollisionShape(CollisionShapeNode* shape, EditorUI* editor);
    void drawBehaviours(Node* node, EditorUI* editor);
};

} // namespace saida
