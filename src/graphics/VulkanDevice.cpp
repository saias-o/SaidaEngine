#include "graphics/VulkanDevice.hpp"

#include "core/Log.hpp"
#include "core/Window.hpp"
#include "graphics/VulkanDeviceCreator.hpp"
#include "rhi/vulkan/CommandEncoder.hpp"
#include "vk_mem_alloc.h"

#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace saida {

namespace {

const std::vector<const char*> kValidationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const std::vector<const char*> kDeviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

// Stored next to the executable (cwd). build/ is gitignored.
constexpr const char* kPipelineCacheFile = "pipeline_cache.bin";

#ifdef NDEBUG
constexpr bool kEnableValidationLayers = false;
#else
constexpr bool kEnableValidationLayers = true;
#endif

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        Log::error("validation: ", data->pMessage);
    else if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        Log::warn("validation: ", data->pMessage);
    return VK_FALSE;
}

VkResult createDebugUtilsMessengerEXT(VkInstance instance,
    const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
    const VkAllocationCallbacks* allocator,
    VkDebugUtilsMessengerEXT* messenger)
{
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    return func ? func(instance, createInfo, allocator, messenger)
                : VK_ERROR_EXTENSION_NOT_PRESENT;
}

void destroyDebugUtilsMessengerEXT(VkInstance instance,
    VkDebugUtilsMessengerEXT messenger,
    const VkAllocationCallbacks* allocator)
{
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func) func(instance, messenger, allocator);
}

void populateDebugMessengerCI(VkDebugUtilsMessengerCreateInfoEXT& ci) {
    ci = {};
    ci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    ci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    ci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                   | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                   | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    ci.pfnUserCallback = debugCallback;
}

} // namespace

VulkanDevice::VulkanDevice(Window& window, VulkanDeviceCreator* creator) : creator_(creator) {
    createInstance();
    setupDebugMessenger();
    createSurface(window);     // no-op in XR mode (no GLFW surface)
    pickPhysicalDevice();
    queryCapabilities();
    createLogicalDevice();
    createAllocator();
    createCommandPools();
    createPipelineCache();
}

VulkanDevice::~VulkanDevice() {
    savePipelineCache();
    vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
    if (computeCommandPool_ != commandPool_)
        vkDestroyCommandPool(device_, computeCommandPool_, nullptr);
    vkDestroyCommandPool(device_, commandPool_, nullptr);
    vmaDestroyAllocator(allocator_);
    vkDestroyDevice(device_, nullptr);
    if (validationEnabled_)
        destroyDebugUtilsMessengerEXT(instance_, debugMessenger_, nullptr);
    if (surface_ != VK_NULL_HANDLE)
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    vkDestroyInstance(instance_, nullptr);
}

// ------------------------------------------------------------------ instance

bool VulkanDevice::checkValidationLayerSupport() const {
    uint32_t count;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> available(count);
    vkEnumerateInstanceLayerProperties(&count, available.data());
    for (const char* name : kValidationLayers) {
        bool found = false;
        for (auto& layer : available)
            if (strcmp(name, layer.layerName) == 0) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

std::vector<const char*> VulkanDevice::requiredInstanceExtensions() const {
    std::vector<const char*> extensions;
    // XR mode presents through OpenXR swapchains, not a GLFW VkSurfaceKHR, so the
    // GLFW surface extensions are not needed (the runtime adds what it requires).
    if (!creator_) {
        uint32_t glfwExtCount = 0;
        const char** glfwExts = glfwGetRequiredInstanceExtensions(&glfwExtCount);
        if (!glfwExts || glfwExtCount == 0)
            throw std::runtime_error(
                "GLFW did not report the Vulkan surface extensions; "
                "the loader or window backend is unavailable");
        extensions.assign(glfwExts, glfwExts + glfwExtCount);
    }
    if (validationEnabled_)
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    return extensions;
}

void VulkanDevice::createInstance() {
    validationEnabled_ = kEnableValidationLayers && checkValidationLayerSupport();
    if (kEnableValidationLayers && !validationEnabled_)
        Log::warn("validation layers requested but not available, continuing without them");

    // Request Vulkan 1.3 when the loader supports it (for dynamic rendering,
    // synchronization2, timeline semaphores…), capped at what's available.
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS)
        loaderVersion = VK_API_VERSION_1_0;
    uint32_t requestedApi = loaderVersion >= VK_API_VERSION_1_3
                          ? VK_API_VERSION_1_3 : loaderVersion;

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "SaidaEngine";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "SaidaEngine";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = requestedApi;

    auto extensions = requiredInstanceExtensions();

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCI{};
    if (validationEnabled_) {
        createInfo.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        createInfo.ppEnabledLayerNames = kValidationLayers.data();
        populateDebugMessengerCI(debugCI);
        createInfo.pNext = &debugCI;
    }

    if (creator_) {
        // OpenXR/Custom creator creates the instance.
        instance_ = creator_->createInstance(createInfo);
    } else {
        const VkResult result = vkCreateInstance(&createInfo, nullptr, &instance_);
        if (result != VK_SUCCESS) {
            std::ostringstream error;
            error << "failed to create Vulkan instance (VkResult "
                  << static_cast<int>(result) << ", API "
                  << VK_API_VERSION_MAJOR(requestedApi) << "."
                  << VK_API_VERSION_MINOR(requestedApi) << ", extensions:";
            for (const char* extension : extensions)
                error << " " << extension;
            error << ")";
            throw std::runtime_error(error.str());
        }
    }
}

void VulkanDevice::setupDebugMessenger() {
    if (!validationEnabled_) return;
    VkDebugUtilsMessengerCreateInfoEXT ci;
    populateDebugMessengerCI(ci);
    if (createDebugUtilsMessengerEXT(instance_, &ci, nullptr, &debugMessenger_) != VK_SUCCESS)
        throw std::runtime_error("failed to set up debug messenger");
}

void VulkanDevice::createSurface(Window& window) {
    if (creator_ && !creator_->requiresSurface()) return;  // XR presents through OpenXR swapchains, not a window surface
    surface_ = window.createSurface(instance_);
}

// ----------------------------------------------------------- physical device

QueueFamilyIndices VulkanDevice::findQueueFamilies(VkPhysicalDevice dev) const {
    QueueFamilyIndices idx;
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(dev, &count, families.data());

    for (uint32_t i = 0; i < count; i++) {
        const VkQueueFlags flags = families[i].queueFlags;
        if (flags & VK_QUEUE_GRAPHICS_BIT && !idx.graphicsFamily.has_value())
            idx.graphicsFamily = i;
        // XR mode has no surface: present goes through OpenXR, so the "present"
        // family is just the graphics family (kept consistent for queue creation).
        if (surface_ != VK_NULL_HANDLE) {
            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, surface_, &presentSupport);
            if (presentSupport && !idx.presentFamily.has_value()) idx.presentFamily = i;
        } else if ((flags & VK_QUEUE_GRAPHICS_BIT) && !idx.presentFamily.has_value()) {
            idx.presentFamily = i;
        }

        // Prefer a dedicated compute family (compute but NOT graphics) for true
        // async compute; otherwise remember any compute-capable family.
        if (flags & VK_QUEUE_COMPUTE_BIT) {
            if (!(flags & VK_QUEUE_GRAPHICS_BIT)) {
                idx.computeFamily = i;
                idx.computeIsDedicated = true;
            } else if (!idx.computeFamily.has_value()) {
                idx.computeFamily = i;  // fallback: graphics+compute family
            }
        }
    }
    return idx;
}

bool VulkanDevice::checkDeviceExtensionSupport(VkPhysicalDevice dev) const {
    uint32_t count;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, available.data());
    std::set<std::string> required(kDeviceExtensions.begin(), kDeviceExtensions.end());
    for (auto& ext : available) required.erase(ext.extensionName);
    return required.empty();
}

SwapChainSupportDetails VulkanDevice::querySwapChainSupport(VkPhysicalDevice dev) const {
    SwapChainSupportDetails details;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(dev, surface_, &details.capabilities);
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &formatCount, nullptr);
    if (formatCount) {
        details.formats.resize(formatCount);
        vkGetPhysicalDeviceSurfaceFormatsKHR(dev, surface_, &formatCount, details.formats.data());
    }
    uint32_t modeCount;
    vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &modeCount, nullptr);
    if (modeCount) {
        details.presentModes.resize(modeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(dev, surface_, &modeCount, details.presentModes.data());
    }
    return details;
}

bool VulkanDevice::isDeviceSuitable(VkPhysicalDevice dev) const {
    auto idx = findQueueFamilies(dev);
    bool extOk = checkDeviceExtensionSupport(dev);
    bool swapOk = false;
    if (extOk) {
        auto sc = querySwapChainSupport(dev);
        swapOk = !sc.formats.empty() && !sc.presentModes.empty();
    }
    return idx.isComplete() && extOk && swapOk;
}

void VulkanDevice::pickPhysicalDevice() {
    if (creator_) {
        // OpenXR/Custom creator mandates the GPU selection.
        physicalDevice_ = creator_->pickPhysicalDevice(instance_);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physicalDevice_, &props);
        Log::info("GPU (Custom): ", props.deviceName);
        return;
    }

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) throw std::runtime_error("no GPU with Vulkan support");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    // Prefer a discrete GPU, otherwise fall back to the first suitable one.
    for (auto& dev : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(dev, &props);
        if (isDeviceSuitable(dev) && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physicalDevice_ = dev;
            Log::info("GPU: ", props.deviceName);
            return;
        }
    }
    for (auto& dev : devices) {
        if (isDeviceSuitable(dev)) {
            physicalDevice_ = dev;
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(dev, &props);
            Log::info("GPU: ", props.deviceName);
            return;
        }
    }
    throw std::runtime_error("no suitable GPU found");
}

// ------------------------------------------------------------- capabilities

void VulkanDevice::queryCapabilities() {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);

    // Negotiated API version = min(loader, requested 1.3, device).
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion(&loaderVersion) != VK_SUCCESS)
        loaderVersion = VK_API_VERSION_1_0;
    uint32_t api = loaderVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : loaderVersion;
    if (props.apiVersion < api) api = props.apiVersion;
    capabilities_.apiVersion = api;
    capabilities_.discreteGpu = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    capabilities_.maxSamples = static_cast<uint32_t>(maxUsableSampleCount());

    // Modern feature query (core Vulkan 1.1/1.2/1.3 feature structs). Only valid
    // to chain when the device exposes at least 1.3; otherwise leave caps false.
    if (api >= VK_API_VERSION_1_3) {
        VkPhysicalDeviceVulkan11Features f11{}; f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
        VkPhysicalDeviceVulkan12Features f12{}; f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
        VkPhysicalDeviceVulkan13Features f13{}; f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        VkPhysicalDeviceFeatures2 f2{}; f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f13; f13.pNext = &f12; f12.pNext = &f11;
        vkGetPhysicalDeviceFeatures2(physicalDevice_, &f2);

        capabilities_.dynamicRendering = f13.dynamicRendering;
        capabilities_.synchronization2 = f13.synchronization2;
        capabilities_.timelineSemaphore = f12.timelineSemaphore;
        capabilities_.descriptorIndexing = f12.descriptorIndexing
                                         && f12.runtimeDescriptorArray
                                         && f12.shaderSampledImageArrayNonUniformIndexing;
        capabilities_.bufferDeviceAddress = f12.bufferDeviceAddress;
        capabilities_.drawIndirectCount = f12.drawIndirectCount;
        capabilities_.multiview = f11.multiview;
    }
    
    // Physical device base features
    VkPhysicalDeviceFeatures baseFeatures{};
    vkGetPhysicalDeviceFeatures(physicalDevice_, &baseFeatures);
    capabilities_.multiDrawIndirect = baseFeatures.multiDrawIndirect;

    // Hardware ray tracing: detected (for the desktop "Ultra" tier), not enabled
    // yet — enabling it pulls in acceleration structures (future work).
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> exts(count);
        vkEnumerateDeviceExtensionProperties(physicalDevice_, nullptr, &count, exts.data());
        bool rayQueryExt = false, accelExt = false;
        for (auto& e : exts) {
            if (strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) rayQueryExt = true;
            if (strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) accelExt = true;
        }
        capabilities_.rayQuery = rayQueryExt && accelExt;
    }

    capabilities_.dedicatedComputeQueue = findQueueFamilies(physicalDevice_).computeIsDedicated;

    // Tier heuristic (combinable at runtime via descriptor/pipeline switching).
    if (capabilities_.discreteGpu && capabilities_.rayQuery)
        capabilities_.tier = QualityTier::Ultra;
    else if (capabilities_.discreteGpu)
        capabilities_.tier = QualityTier::High;
    else if (capabilities_.dynamicRendering)
        capabilities_.tier = QualityTier::Medium;
    else
        capabilities_.tier = QualityTier::Low;

    Log::info("Vulkan ", VK_API_VERSION_MAJOR(api), ".", VK_API_VERSION_MINOR(api),
              " | tier ", toString(capabilities_.tier),
              " | dynRender=", capabilities_.dynamicRendering,
              " sync2=", capabilities_.synchronization2,
              " timeline=", capabilities_.timelineSemaphore,
              " bindless=", capabilities_.descriptorIndexing,
              " multiview=", capabilities_.multiview,
              " rayQuery=", capabilities_.rayQuery,
              " asyncCompute=", capabilities_.dedicatedComputeQueue);
}

// ------------------------------------------------------------ logical device

void VulkanDevice::createLogicalDevice() {
    auto idx = findQueueFamilies(physicalDevice_);
    computeFamily_ = idx.computeFamily.value_or(idx.graphicsFamily.value());

    std::set<uint32_t> uniqueFamilies = {
        idx.graphicsFamily.value(), idx.presentFamily.value(), computeFamily_};

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCIs;
    for (uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = family;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        queueCIs.push_back(qci);
    }

    VkPhysicalDeviceFeatures features{};
    features.samplerAnisotropy = VK_TRUE;

    VkDeviceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = static_cast<uint32_t>(queueCIs.size());
    ci.pQueueCreateInfos = queueCIs.data();
    ci.enabledExtensionCount = static_cast<uint32_t>(kDeviceExtensions.size());
    ci.ppEnabledExtensionNames = kDeviceExtensions.data();
    if (validationEnabled_) {
        ci.enabledLayerCount = static_cast<uint32_t>(kValidationLayers.size());
        ci.ppEnabledLayerNames = kValidationLayers.data();
    }

    // Enable the modern core features we detected (gated). Declared here so they
    // outlive vkCreateDevice. Falls back to plain 1.0 device creation otherwise.
    VkPhysicalDeviceVulkan11Features e11{}; e11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    VkPhysicalDeviceVulkan12Features e12{}; e12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    VkPhysicalDeviceVulkan13Features e13{}; e13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceFeatures2 e2{}; e2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    if (capabilities_.apiVersion >= VK_API_VERSION_1_3) {
        e2.pNext = &e13; e13.pNext = &e12; e12.pNext = &e11;
        e13.dynamicRendering = capabilities_.dynamicRendering;
        e13.synchronization2 = capabilities_.synchronization2;
        e12.timelineSemaphore = capabilities_.timelineSemaphore;
        e12.descriptorIndexing = capabilities_.descriptorIndexing;
        e12.runtimeDescriptorArray = capabilities_.descriptorIndexing;
        e12.shaderSampledImageArrayNonUniformIndexing = capabilities_.descriptorIndexing;
        e12.descriptorBindingPartiallyBound = capabilities_.descriptorIndexing;
        e12.bufferDeviceAddress = capabilities_.bufferDeviceAddress;
        e12.drawIndirectCount = capabilities_.drawIndirectCount;
        e11.multiview = capabilities_.multiview;
        e2.features.samplerAnisotropy = VK_TRUE;
        e2.features.multiDrawIndirect = capabilities_.multiDrawIndirect;
        ci.pNext = &e2;  // when using features2, pEnabledFeatures must be null
    } else {
        ci.pEnabledFeatures = &features;
    }

    if (creator_) {
        // OpenXR/Custom creator creates the device.
        device_ = creator_->createDevice(physicalDevice_, ci);
    } else if (vkCreateDevice(physicalDevice_, &ci, nullptr, &device_) != VK_SUCCESS) {
        throw std::runtime_error("failed to create logical device");
    }

    vkGetDeviceQueue(device_, idx.graphicsFamily.value(), 0, &graphicsQueue_);
    vkGetDeviceQueue(device_, idx.presentFamily.value(), 0, &presentQueue_);
    vkGetDeviceQueue(device_, computeFamily_, 0, &computeQueue_);
}

void VulkanDevice::createCommandPools() {
    auto idx = findQueueFamilies(physicalDevice_);

    VkCommandPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    ci.queueFamilyIndex = idx.graphicsFamily.value();
    if (vkCreateCommandPool(device_, &ci, nullptr, &commandPool_) != VK_SUCCESS)
        throw std::runtime_error("failed to create command pool");

    // Separate pool for the async-compute family, or reuse the graphics pool when
    // there's no distinct compute family.
    if (computeFamily_ != idx.graphicsFamily.value()) {
        VkCommandPoolCreateInfo cci = ci;
        cci.queueFamilyIndex = computeFamily_;
        if (vkCreateCommandPool(device_, &cci, nullptr, &computeCommandPool_) != VK_SUCCESS)
            throw std::runtime_error("failed to create compute command pool");
    } else {
        computeCommandPool_ = commandPool_;
    }
}

void VulkanDevice::createAllocator() {
    VmaAllocatorCreateInfo ci{};
    ci.physicalDevice = physicalDevice_;
    ci.device = device_;
    ci.instance = instance_;
    ci.vulkanApiVersion = capabilities_.apiVersion;
    if (capabilities_.bufferDeviceAddress)
        ci.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    if (vmaCreateAllocator(&ci, &allocator_) != VK_SUCCESS)
        throw std::runtime_error("failed to create memory allocator");
}

void VulkanDevice::createPipelineCache() {
    // Seed the cache from disk if a previous run saved one (faster pipeline
    // creation; Vulkan validates the header and ignores incompatible data).
    std::vector<char> initialData;
    if (std::ifstream file(kPipelineCacheFile, std::ios::binary | std::ios::ate); file) {
        initialData.resize(static_cast<size_t>(file.tellg()));
        file.seekg(0);
        file.read(initialData.data(), static_cast<std::streamsize>(initialData.size()));
    }

    VkPipelineCacheCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = initialData.size();
    ci.pInitialData = initialData.empty() ? nullptr : initialData.data();
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipelineCache_) != VK_SUCCESS)
        throw std::runtime_error("failed to create pipeline cache");
}

void VulkanDevice::savePipelineCache() const {
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, nullptr) != VK_SUCCESS || size == 0)
        return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device_, pipelineCache_, &size, data.data()) != VK_SUCCESS)
        return;
    if (std::ofstream file(kPipelineCacheFile, std::ios::binary); file)
        file.write(data.data(), static_cast<std::streamsize>(size));
}

// ----------------------------------------------------------------- helpers

VkSampleCountFlagBits VulkanDevice::maxUsableSampleCount() const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    VkSampleCountFlags counts = props.limits.framebufferColorSampleCounts
                              & props.limits.framebufferDepthSampleCounts;
    // Cap at 4x: a good quality/perf trade-off for this engine.
    if (counts & VK_SAMPLE_COUNT_4_BIT) return VK_SAMPLE_COUNT_4_BIT;
    if (counts & VK_SAMPLE_COUNT_2_BIT) return VK_SAMPLE_COUNT_2_BIT;
    return VK_SAMPLE_COUNT_1_BIT;
}

float VulkanDevice::maxAnisotropy() const {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physicalDevice_, &props);
    return props.limits.maxSamplerAnisotropy;
}

VkFormat VulkanDevice::findSupportedFormat(const std::vector<VkFormat>& candidates,
    VkImageTiling tiling, VkFormatFeatureFlags features) const
{
    for (VkFormat f : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(physicalDevice_, f, &props);
        if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
            return f;
        if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
            return f;
    }
    throw std::runtime_error("failed to find supported format");
}

VkFormat VulkanDevice::findDepthFormat() const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

void VulkanDevice::withSingleTimeEncoder(
    const std::function<void(rhi::vulkan::CommandEncoder&)>& fn) const {
    rhi::vulkan::CommandEncoder encoder(beginSingleTimeCommands());
    fn(encoder);
    endSingleTimeCommands(encoder.handle());
}

VkImageView VulkanDevice::createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspect) const {
    VkImageViewCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ci.image = image;
    ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ci.format = format;
    ci.subresourceRange.aspectMask = aspect;
    ci.subresourceRange.baseMipLevel = 0;
    ci.subresourceRange.levelCount = 1;
    ci.subresourceRange.baseArrayLayer = 0;
    ci.subresourceRange.layerCount = 1;
    VkImageView view;
    if (vkCreateImageView(device_, &ci, nullptr, &view) != VK_SUCCESS)
        throw std::runtime_error("failed to create image view");
    return view;
}

VkCommandBuffer VulkanDevice::beginSingleTimeCommands() const {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool_;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);
    return cmd;
}

void VulkanDevice::endSingleTimeCommands(VkCommandBuffer cmd) const {
    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(graphicsQueue_, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue_);

    vkFreeCommandBuffers(device_, commandPool_, 1, &cmd);
}

} // namespace saida
