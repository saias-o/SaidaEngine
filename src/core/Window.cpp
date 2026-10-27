#include "core/Window.hpp"

#include <algorithm>
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
    // Key state is polled (glfwGetKey); the callback only reads the lock
    // modifiers, which GLFW reports with key events alone.
    glfwSetInputMode(window_, GLFW_LOCK_KEY_MODS, GLFW_TRUE);
    glfwSetKeyCallback(window_, keyCallback);

    savedWidth_ = width;
    savedHeight_ = height;
    glfwGetWindowPos(window_, &savedX_, &savedY_);
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

void Window::keyCallback(GLFWwindow* window, int, int, int, int mods) {
    auto self = reinterpret_cast<Window*>(glfwGetWindowUserPointer(window));
    self->lockMods_ = mods & (GLFW_MOD_CAPS_LOCK | GLFW_MOD_NUM_LOCK);
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

GLFWmonitor* Window::currentMonitor() const {
    int monitorCount = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&monitorCount);
    if (!monitors || monitorCount == 0)
        return glfwGetPrimaryMonitor();

    int wx = 0, wy = 0, ww = 0, wh = 0;
    glfwGetWindowPos(window_, &wx, &wy);
    glfwGetWindowSize(window_, &ww, &wh);

    GLFWmonitor* best = glfwGetPrimaryMonitor();
    int maxOverlap = 0;

    for (int i = 0; i < monitorCount; ++i) {
        int mx = 0, my = 0;
        glfwGetMonitorPos(monitors[i], &mx, &my);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (!mode) continue;

        const int overlapX = std::max(0, std::min(wx + ww, mx + mode->width) - std::max(wx, mx));
        const int overlapY = std::max(0, std::min(wy + wh, my + mode->height) - std::max(wy, my));
        const int overlap = overlapX * overlapY;

        if (overlap > maxOverlap) {
            maxOverlap = overlap;
            best = monitors[i];
        }
    }
    return best;
}

bool Window::isFullscreen() const {
    if (!window_) return isFullscreen_;
    if (glfwGetWindowAttrib(window_, GLFW_VISIBLE) == GLFW_FALSE) return isFullscreen_;
    return glfwGetWindowMonitor(window_) != nullptr;
}

void Window::setFullscreen(bool enable) {
    if (isFullscreen() == enable) return;

    if (!window_) {
        isFullscreen_ = enable;
        return;
    }

    if (glfwGetWindowAttrib(window_, GLFW_VISIBLE) == GLFW_FALSE) {
        isFullscreen_ = enable;
        resized_ = true;
        return;
    }

    if (enable) {
        int x = 0, y = 0, w = 0, h = 0;
        glfwGetWindowPos(window_, &x, &y);
        glfwGetWindowSize(window_, &w, &h);
        if (w > 0 && h > 0) {
            savedX_ = x;
            savedY_ = y;
            savedWidth_ = w;
            savedHeight_ = h;
        }

        GLFWmonitor* monitor = currentMonitor();
        if (!monitor) monitor = glfwGetPrimaryMonitor();
        if (monitor) {
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            if (mode) {
                glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
                isFullscreen_ = true;
                resized_ = true;
            }
        }
    } else {
        const int targetW = savedWidth_ > 0 ? savedWidth_ : 1600;
        const int targetH = savedHeight_ > 0 ? savedHeight_ : 900;
        glfwSetWindowMonitor(window_, nullptr, savedX_, savedY_, targetW, targetH, 0);
        isFullscreen_ = false;
        resized_ = true;
    }
}

} // namespace saida
