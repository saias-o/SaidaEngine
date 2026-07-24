#pragma once

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <cstdint>
#include <string>
#include <vector>

namespace saida {

// Owns the GLFW window and reports framebuffer resize events.
class Window {
public:
    Window(int width, int height, std::string title, bool visible = true);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool shouldClose() const { return glfwWindowShouldClose(window_); }
    void pollEvents() const { glfwPollEvents(); }
    void waitEvents() const { glfwWaitEvents(); }

    VkSurfaceKHR createSurface(VkInstance instance) const;
    void framebufferSize(int& width, int& height) const { glfwGetFramebufferSize(window_, &width, &height); }

    bool wasResized() const { return resized_; }
    void resetResizedFlag() { resized_ = false; }

    void close() { glfwSetWindowShouldClose(window_, GLFW_TRUE); }

    // --- input ---
    bool keyDown(int glfwKey) const { return glfwGetKey(window_, glfwKey) == GLFW_PRESS; }
    bool mouseButtonDown(int glfwButton) const { return glfwGetMouseButton(window_, glfwButton) == GLFW_PRESS; }
    // Mouse movement accumulated since the last call (then reset to zero).
    void consumeMouseDelta(double& dx, double& dy);
    void consumeScrollDelta(double& dx, double& dy);
    std::vector<uint32_t> consumeTextInput();

    // Cursor capture: disabled (hidden, locked) for fly-cam vs normal for UI.
    bool cursorCaptured() const { return cursorCaptured_; }
    void setCursorCaptured(bool captured);

    GLFWwindow* handle() const { return window_; }

private:
    static void framebufferResizeCallback(GLFWwindow* window, int width, int height);
    static void cursorPosCallback(GLFWwindow* window, double x, double y);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);
    static void charCallback(GLFWwindow* window, unsigned int codepoint);

    GLFWwindow* window_ = nullptr;
    bool resized_ = false;

    double mouseDx_ = 0.0;
    double mouseDy_ = 0.0;
    double scrollDx_ = 0.0;
    double scrollDy_ = 0.0;
    std::vector<uint32_t> textInput_;
    double lastX_ = 0.0;
    double lastY_ = 0.0;
    bool firstMouse_ = true;
    bool cursorCaptured_ = false;
};

} // namespace saida
