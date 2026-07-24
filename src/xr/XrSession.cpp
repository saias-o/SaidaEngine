#include "xr/XrPlatform.hpp"   // must precede the XR headers (include order)
#include "xr/XrSession.hpp"
#include "xr/XrInstance.hpp"
#include "xr/XrSwapchain.hpp"
#include "xr/XrActions.hpp"
#include "xr/XrHandTracking.hpp"
#include "xr/XrMath.hpp"

#include "graphics/VulkanDevice.hpp"
#include "xr/toolkit/XRPassthrough.hpp"
#include "core/Log.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

namespace saida::xr {

Session::Session(Instance& instance, VulkanDevice& device)
    : instance_(instance), device_(device) {
    createSession();
    selectReferenceSpace();
    createReferenceSpace();
    enumerateViewConfig();
    enumerateBlendModes();
    colorFormat_ = chooseColorFormat();
    createSwapchains();
    createCommandResources();
    actions_ = std::make_unique<Actions>(instance_, session_);  // feeds saida::XRInput
    if (instance_.handTrackingSupported()) {
        try {
            handTracking_ = std::make_unique<HandTracking>(instance_, session_);
            Log::info("XR hand trackers ready");
        } catch (const std::exception& e) {
            Log::warn("XR hand tracking initialization failed: ", e.what());
        }
    }
    Log::info("XR session ready: ", viewCount(), " views, ",
              swapchains_[0]->width(), "x", swapchains_[0]->height(), " per eye");
}

Session::~Session() {
    if (session_ != XR_NULL_HANDLE) vkDeviceWaitIdle(device_.device());
    handTracking_.reset(); // native trackers must die before the session
    actions_.reset();  // destroy action spaces/set before the session
    for (VkFence f : fences_)
        if (f) vkDestroyFence(device_.device(), f, nullptr);
    if (!cmdBuffers_.empty())
        vkFreeCommandBuffers(device_.device(), device_.commandPool(),
                             static_cast<uint32_t>(cmdBuffers_.size()), cmdBuffers_.data());
    swapchains_.clear();  // destroy XR swapchains (and their image views) first
    if (appSpace_ != XR_NULL_HANDLE) xrDestroySpace(appSpace_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
}

void Session::createSession() {
    // The graphics binding shares the engine's Vulkan device/queue with the
    // runtime (graphics family, queue 0 — the queue the engine renders on).
    XrGraphicsBindingVulkan2KHR binding{};
    binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
    binding.instance = device_.instance();
    binding.physicalDevice = device_.physicalDevice();
    binding.device = device_.device();
    binding.queueFamilyIndex = device_.findQueueFamilies().graphicsFamily.value();
    binding.queueIndex = 0;

    XrSessionCreateInfo ci{};
    ci.type = XR_TYPE_SESSION_CREATE_INFO;
    ci.next = &binding;
    ci.systemId = instance_.systemId();
    check(xrCreateSession(instance_.handle(), &ci, &session_), "xrCreateSession");
}

void Session::selectReferenceSpace() {
    uint32_t count = 0;
    check(xrEnumerateReferenceSpaces(session_, 0, &count, nullptr),
          "xrEnumerateReferenceSpaces(count)");
    std::vector<XrReferenceSpaceType> spaces(count);
    check(xrEnumerateReferenceSpaces(session_, count, &count, spaces.data()),
          "xrEnumerateReferenceSpaces");
    appSpaceType_ = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (XrReferenceSpaceType type : spaces)
        if (type == XR_REFERENCE_SPACE_TYPE_STAGE) {
            appSpaceType_ = type;
            break;
        }
    Log::info("XR reference space: ",
              appSpaceType_ == XR_REFERENCE_SPACE_TYPE_STAGE ?
                  "STAGE (room-scale)" : "LOCAL");
}

void Session::createReferenceSpace() {
    // Prefer STAGE so physical movement is expressed in a floor-level room-scale
    // space. LOCAL remains the mandatory fallback. The rig offset is baked into
    // poseInReferenceSpace so teleport/snap-turn still recenter the whole rig.
    if (appSpace_ != XR_NULL_HANDLE) {
        xrDestroySpace(appSpace_);
        appSpace_ = XR_NULL_HANDLE;
    }
    const float half = originYaw_ * 0.5f;
    XrReferenceSpaceCreateInfo ci{};
    ci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    ci.referenceSpaceType = appSpaceType_;
    ci.poseInReferenceSpace.orientation.x = 0.0f;
    ci.poseInReferenceSpace.orientation.y = std::sin(half);
    ci.poseInReferenceSpace.orientation.z = 0.0f;
    ci.poseInReferenceSpace.orientation.w = std::cos(half);
    ci.poseInReferenceSpace.position = {originPos_.x, originPos_.y, originPos_.z};
    check(xrCreateReferenceSpace(session_, &ci, &appSpace_), "xrCreateReferenceSpace");
}

void Session::setReferenceOffset(const glm::vec3& position, float yawRadians) {
    if (position == originPos_ && yawRadians == originYaw_) return;
    originPos_ = position;
    originYaw_ = yawRadians;
    if (session_ != XR_NULL_HANDLE) createReferenceSpace();
}

void Session::syncActions() {
    if (!running_) return;
    if (actions_) actions_->sync(session_, appSpace_, lastDisplayTime_);
    if (handTracking_) handTracking_->sync(appSpace_, lastDisplayTime_);
}

void Session::enumerateViewConfig() {
    uint32_t count = 0;
    check(xrEnumerateViewConfigurationViews(instance_.handle(), instance_.systemId(),
              kViewConfig, 0, &count, nullptr),
          "xrEnumerateViewConfigurationViews(count)");
    XrViewConfigurationView tmpl{};
    tmpl.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    viewConfigs_.assign(count, tmpl);
    check(xrEnumerateViewConfigurationViews(instance_.handle(), instance_.systemId(),
              kViewConfig, count, &count, viewConfigs_.data()),
          "xrEnumerateViewConfigurationViews");
}

void Session::enumerateBlendModes() {
    uint32_t count = 0;
    check(xrEnumerateEnvironmentBlendModes(instance_.handle(), instance_.systemId(),
              kViewConfig, 0, &count, nullptr),
          "xrEnumerateEnvironmentBlendModes(count)");
    std::vector<XrEnvironmentBlendMode> modes(count);
    check(xrEnumerateEnvironmentBlendModes(instance_.handle(), instance_.systemId(),
              kViewConfig, count, &count, modes.data()),
          "xrEnumerateEnvironmentBlendModes");

    // Prefer ALPHA_BLEND for passthrough (additive as a fallback). OPAQUE stays
    // the default for VR. The toolkit's XRPassthrough service reads support back.
    for (XrEnvironmentBlendMode m : modes) {
        if (m == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND) {
            passthroughMode_ = m;
            passthroughSupported_ = true;
            break;
        }
        if (m == XR_ENVIRONMENT_BLEND_MODE_ADDITIVE && !passthroughSupported_) {
            passthroughMode_ = m;
            passthroughSupported_ = true;
        }
    }
    XRPassthrough::setSupported(passthroughSupported_);
    Log::info("XR passthrough ", passthroughSupported_ ? "supported" : "unsupported");
}

int64_t Session::chooseColorFormat() const {
    uint32_t count = 0;
    check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr),
          "xrEnumerateSwapchainFormats(count)");
    std::vector<int64_t> formats(count);
    check(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()),
          "xrEnumerateSwapchainFormats");

    // Prefer an sRGB 8-bit format: the compositor expects sRGB-encoded color, and
    // the tonemap pass writes LDR sRGB just like the desktop swapchain.
    const VkFormat preferred[] = {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB};
    for (VkFormat want : preferred)
        for (int64_t have : formats)
            if (have == static_cast<int64_t>(want)) return have;
    if (formats.empty()) throw std::runtime_error("OpenXR: no swapchain formats");
    return formats[0];  // fall back to the runtime's first preference
}

void Session::createSwapchains() {
    swapchains_.reserve(viewConfigs_.size());
    for (const auto& vc : viewConfigs_)
        swapchains_.push_back(std::make_unique<Swapchain>(
            device_, session_, colorFormat_,
            vc.recommendedImageRectWidth, vc.recommendedImageRectHeight,
            VK_SAMPLE_COUNT_1_BIT));  // MSAA is resolved upstream by the renderer
}

void Session::createCommandResources() {
    cmdBuffers_.resize(kFrameSlots);
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = device_.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = kFrameSlots;
    if (vkAllocateCommandBuffers(device_.device(), &ai, cmdBuffers_.data()) != VK_SUCCESS)
        throw std::runtime_error("XR: failed to allocate command buffers");

    fences_.resize(kFrameSlots);
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (auto& f : fences_)
        if (vkCreateFence(device_.device(), &fi, nullptr, &f) != VK_SUCCESS)
            throw std::runtime_error("XR: failed to create fence");
}

bool Session::pollEvents() {
    XrEventDataBuffer ev{};
    ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(instance_.handle(), &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
                onSessionStateChanged(e.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                exitRequested_ = true;
                break;
            default:
                break;
        }
        ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
    return !exitRequested_;
}

void Session::onSessionStateChanged(XrSessionState state) {
    state_ = state;
    switch (state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo bi{};
            bi.type = XR_TYPE_SESSION_BEGIN_INFO;
            bi.primaryViewConfigurationType = kViewConfig;
            check(xrBeginSession(session_, &bi), "xrBeginSession");
            running_ = true;
            Log::info("XR session running");
            break;
        }
        case XR_SESSION_STATE_STOPPING:
            check(xrEndSession(session_), "xrEndSession");
            running_ = false;
            break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            exitRequested_ = true;
            running_ = false;
            break;
        default:
            break;
    }
}

VkExtent2D Session::eyeExtent() const {
    if (swapchains_.empty()) return {0, 0};
    return {swapchains_[0]->width(), swapchains_[0]->height()};
}

void Session::renderFrame(const RenderFrameFn& render) {
    if (!running_) return;

    XrFrameWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    check(xrWaitFrame(session_, &waitInfo, &frameState), "xrWaitFrame");
    lastDisplayTime_ = frameState.predictedDisplayTime;  // for next frame's action poses

    XrFrameBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    check(xrBeginFrame(session_, &beginInfo), "xrBeginFrame");

    const uint32_t count = viewCount();
    XrCompositionLayerProjectionView projViewTmpl{};
    projViewTmpl.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
    std::vector<XrCompositionLayerProjectionView> projViews(count, projViewTmpl);
    XrCompositionLayerProjection layer{};
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;

    bool submittedLayer = false;
    if (frameState.shouldRender == XR_TRUE) {
        // Locate the eye poses + FOVs for this frame's predicted display time.
        XrView viewTmpl{};
        viewTmpl.type = XR_TYPE_VIEW;
        std::vector<XrView> views(count, viewTmpl);
        XrViewState viewState{};
        viewState.type = XR_TYPE_VIEW_STATE;
        XrViewLocateInfo locate{};
        locate.type = XR_TYPE_VIEW_LOCATE_INFO;
        locate.viewConfigurationType = kViewConfig;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = appSpace_;
        uint32_t located = 0;
        check(xrLocateViews(session_, &locate, &viewState, count, &located, views.data()),
              "xrLocateViews");

        const bool posesValid =
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);

        if (posesValid) {
            VkCommandBuffer cmd = cmdBuffers_[frameSlot_];
            VkFence fence = fences_[frameSlot_];
            vkWaitForFences(device_.device(), 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(device_.device(), 1, &fence);
            vkResetCommandBuffer(cmd, 0);

            VkCommandBufferBeginInfo cbi{};
            cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &cbi);

            // Acquire + wait every eye image up front, then record the whole stereo
            // frame in one callback (the renderer draws both eyes via multiview).
            std::vector<uint32_t> imageIndices(count);
            std::vector<EyeView> eyes(count);
            for (uint32_t i = 0; i < count; ++i) {
                Swapchain& sc = *swapchains_[i];
                imageIndices[i] = sc.acquire();
                sc.wait();

                EyeView& eye = eyes[i];
                eye.image = sc.image(imageIndices[i]);
                eye.imageView = sc.view(imageIndices[i]);
                eye.extent = sc.extent();
                eye.view = viewFromPose(views[i].pose);
                eye.projection = projectionFromFov(views[i].fov, nearZ_, farZ_);
                eye.eyePosition = toGlm(views[i].pose.position);

                projViews[i].pose = views[i].pose;
                projViews[i].fov = views[i].fov;
                projViews[i].subImage.swapchain = sc.handle();
                projViews[i].subImage.imageRect.offset = {0, 0};
                projViews[i].subImage.imageRect.extent = {
                    static_cast<int32_t>(sc.width()), static_cast<int32_t>(sc.height())};
                projViews[i].subImage.imageArrayIndex = 0;
            }

            render(cmd, eyes);

            vkEndCommandBuffer(cmd);
            VkSubmitInfo submit{};
            submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &cmd;
            if (vkQueueSubmit(device_.graphicsQueue(), 1, &submit, fence) != VK_SUCCESS)
                throw std::runtime_error("XR: failed to submit eye render");
            // The compositor reads the released images, so ensure the GPU is done.
            vkWaitForFences(device_.device(), 1, &fence, VK_TRUE, UINT64_MAX);

            for (uint32_t i = 0; i < count; ++i)
                swapchains_[i]->release();

            // Head pose = centroid of the eyes; orientation from the first view.
            glm::vec3 centroid(0.0f);
            for (uint32_t i = 0; i < count; ++i)
                centroid += toGlm(views[i].pose.position);
            headPosition_ = centroid / static_cast<float>(count);
            headOrientation_ = toGlm(views[0].pose.orientation);
            headView_ = viewFromPose(views[0].pose);

            layer.space = appSpace_;
            layer.viewCount = count;
            layer.views = projViews.data();
            submittedLayer = true;
            frameSlot_ = (frameSlot_ + 1) % kFrameSlots;
        }
    }

    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = frameState.predictedDisplayTime;
    // Passthrough (AR) when gameplay enabled it and the runtime supports it,
    // otherwise opaque (VR). The renderer clears transparent for passthrough.
    endInfo.environmentBlendMode =
        XRPassthrough::enabled() ? passthroughMode_ : opaqueMode_;
    const XrCompositionLayerBaseHeader* layers[] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    endInfo.layerCount = submittedLayer ? 1 : 0;
    endInfo.layers = submittedLayer ? layers : nullptr;
    check(xrEndFrame(session_, &endInfo), "xrEndFrame");
}

} // namespace saida::xr
