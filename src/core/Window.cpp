#include "core/Window.hpp"

#include <stdexcept>
#include <utility>

namespace saida {

Window::Window(int width, int height, std::string title, bool visible) {
    if (glfwInit() != GLFW_TRUE)
        throw std::runtime_error("failed to initialize GLFW");
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, visible ? GLFW_TRUE : GLFW_FALSE);
    window_ = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!window_) {
        const char* description = nullptr;
        glfwGetError(&description);
        const std::string message =
            description ? std::string("failed to create GLFW window: ") + description
                        : "failed to create GLFW window";
        glfwTerminate();
        throw std::runtime_error(message);
    }
    glfwSetWindowUserPointer(window_, this);
    glfwSetFramebufferSizeCallback(window_, framebufferResizeCallback);

    // By default, the cursor is normal for UI interaction.
    glfwSetInputMode(window_, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(window_, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    glfwSetCursorPosCallback(window_, cursorPosCallback);
    glfwSetScrollCallback(window_, scrollCallback);
    glfwSetCharCallback(window_, charCallback);
}

Window::~Window() {
    glfwDestroyWindow(window_);
    glfwTerminate();
}

void Window::framebufferResizeCallback(GLFWwindow* window, int, int) {
    auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    self->resized_ = true;
}

void Window::cursorPosCallback(GLFWwindow* window, double x, double y) {
    auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    if (self->firstMouse_) {
        self->lastX_ = x;
        self->lastY_ = y;
        self->firstMouse_ = false;
    }
    self->mouseDx_ += x - self->lastX_;
    self->mouseDy_ += y - self->lastY_;
    self->lastX_ = x;
    self->lastY_ = y;
}

void Window::scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
    auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    self->scrollDx_ += xoffset;
    self->scrollDy_ += yoffset;
}

void Window::charCallback(GLFWwindow* window, unsigned int codepoint) {
    auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    self->textInput_.push_back(static_cast<uint32_t>(codepoint));
}

void Window::consumeMouseDelta(double& dx, double& dy) {
    dx = mouseDx_;
    dy = mouseDy_;
    mouseDx_ = 0.0;
    mouseDy_ = 0.0;
}

void Window::consumeScrollDelta(double& dx, double& dy) {
    dx = scrollDx_;
    dy = scrollDy_;
    scrollDx_ = 0.0;
    scrollDy_ = 0.0;
}

std::vector<uint32_t> Window::consumeTextInput() {
    std::vector<uint32_t> out = std::move(textInput_);
    textInput_.clear();
    return out;
}

void Window::setCursorCaptured(bool captured) {
    if (captured == cursorCaptured_) return;
    cursorCaptured_ = captured;
    glfwSetInputMode(window_, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    firstMouse_ = true;  // avoid a look jump when re-capturing
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(instance, window_, nullptr, &surface) != VK_SUCCESS)
        throw std::runtime_error("failed to create window surface");
    return surface;
}

} // namespace saida
