#include <vulkan/vulkan.h>

#include <cstring>
#include <iostream>
#include <vector>

namespace {

bool hasExtension(const std::vector<VkExtensionProperties>& extensions,
                  const char* wanted) {
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, wanted) == 0) return true;
    }
    return false;
}

} // namespace

int main() {
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    const VkResult versionResult = vkEnumerateInstanceVersion(&loaderVersion);
    if (versionResult != VK_SUCCESS) {
        std::cerr << "vkEnumerateInstanceVersion failed: "
                  << static_cast<int>(versionResult) << "\n";
        return 1;
    }

    uint32_t extensionCount = 0;
    VkResult result =
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS) {
        std::cerr << "instance extension count failed: "
                  << static_cast<int>(result) << "\n";
        return 1;
    }
    std::vector<VkExtensionProperties> extensions(extensionCount);
    result = vkEnumerateInstanceExtensionProperties(
        nullptr, &extensionCount, extensions.data());
    if (result != VK_SUCCESS) {
        std::cerr << "instance extension enumeration failed: "
                  << static_cast<int>(result) << "\n";
        return 1;
    }

    constexpr const char* required[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        "VK_KHR_win32_surface",
    };
    for (const char* extension : required) {
        if (!hasExtension(extensions, extension)) {
            std::cerr << "missing instance extension: " << extension << "\n";
            return 1;
        }
    }

    const uint32_t requestedApi =
        loaderVersion >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : loaderVersion;
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Saida Vulkan CI preflight";
    application.apiVersion = requestedApi;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &application;
    createInfo.enabledExtensionCount = 2;
    createInfo.ppEnabledExtensionNames = required;

    VkInstance instance = VK_NULL_HANDLE;
    result = vkCreateInstance(&createInfo, nullptr, &instance);
    if (result != VK_SUCCESS) {
        std::cerr << "vkCreateInstance failed: " << static_cast<int>(result)
                  << "\n";
        return 1;
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS || deviceCount == 0) {
        std::cerr << "physical device enumeration failed: "
                  << static_cast<int>(result) << ", count=" << deviceCount << "\n";
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS) {
        std::cerr << "physical device list failed: "
                  << static_cast<int>(result) << "\n";
        vkDestroyInstance(instance, nullptr);
        return 1;
    }

    std::cout << "VULKAN PREFLIGHT PASS: loader "
              << VK_API_VERSION_MAJOR(loaderVersion) << "."
              << VK_API_VERSION_MINOR(loaderVersion) << "."
              << VK_API_VERSION_PATCH(loaderVersion)
              << ", devices=" << deviceCount << "\n";
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        std::cout << "  " << properties.deviceName << " | API "
                  << VK_API_VERSION_MAJOR(properties.apiVersion) << "."
                  << VK_API_VERSION_MINOR(properties.apiVersion) << "."
                  << VK_API_VERSION_PATCH(properties.apiVersion) << "\n";
    }

    vkDestroyInstance(instance, nullptr);
    return 0;
}
