#pragma once

#include "nodes/UINode.hpp"
#include <functional>

namespace saida {

class UIInteractableNode : public UINode {
public:
    enum class State {
        Normal,
        Hover,
        Pressed,
        Disabled
    };

    UIInteractableNode() = default;
    virtual ~UIInteractableNode() = default;

    State state() const { return state_; }
    void setState(State s) { state_ = s; }

    bool interactable() const { return interactable_; }
    void setInteractable(bool interactable) { interactable_ = interactable; }

    virtual void onHoverEnter() { if (interactable_) state_ = State::Hover; }
    virtual void onHoverExit() { if (interactable_) state_ = State::Normal; }
    virtual void onMouseDown() { if (interactable_) state_ = State::Pressed; }
    virtual void onMouseUp() { if (interactable_) state_ = State::Hover; }
    virtual void onClick() { if (onClickCallback_) onClickCallback_(); }

    using ClickCallback = std::function<void()>;
    void setOnClick(ClickCallback cb) { onClickCallback_ = cb; }

    const char* typeName() const override { return "UIInteractableNode"; }
    void serialize(nlohmann::json& j, ResourceManager& resources) const override;
    void deserialize(const nlohmann::json& j, ResourceManager& resources) override;

protected:
    State state_ = State::Normal;
    bool interactable_ = true;
    ClickCallback onClickCallback_;
};

} // namespace saida
