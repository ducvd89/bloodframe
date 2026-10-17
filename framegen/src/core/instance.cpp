#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/instance.hpp"
#include "common/exception.hpp"
#include "lsfg_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using namespace LSFG::Core;

namespace {

std::mutex privateInstanceEnvironmentMutex;

// bbport: the host's own vkGetInstanceProcAddr (setInstanceProcAddr); null: volk's own loader.
std::atomic<PFN_vkGetInstanceProcAddr> hostGetInstanceProcAddr{nullptr};

std::optional<std::string> readEnvironment(const char* name) {
    const char* value = std::getenv(name);
    if (!value)
        return std::nullopt;
    return std::string(value);
}

void restoreEnvironment(const char* name, const std::optional<std::string>& value) {
    if (value.has_value())
        setenv(name, value->c_str(), 1);
    else
        unsetenv(name);
}

std::string stripLayerName(const std::string& value, const std::string& layerName) {
    std::vector<std::string> layers;
    size_t start = 0;
    for (size_t i = 0; i <= value.size(); ++i) {
        const bool atEnd = i == value.size();
        const bool separator = !atEnd
            && (value[i] == ':' || value[i] == ';' || value[i] == ',');
        if (!atEnd && !separator)
            continue;
        if (i > start) {
            const std::string token = value.substr(start, i - start);
            if (token != layerName)
                layers.push_back(token);
        }
        start = i + 1;
    }

    const char joinSeparator =
        value.find(',') != std::string::npos ? ',' : ':';
    std::string result;
    for (const auto& layer : layers) {
        if (!result.empty())
            result += joinSeparator;
        result += layer;
    }
    return result;
}

class ScopedPrivateInstanceLayerSuppression {
public:
    ScopedPrivateInstanceLayerSuppression()
        : lock(privateInstanceEnvironmentMutex),
          previousDisable(readEnvironment("DISABLE_LSFG")),
          previousInstanceLayers(readEnvironment("VK_INSTANCE_LAYERS")),
          previousLoaderLayersEnable(readEnvironment("VK_LOADER_LAYERS_ENABLE")) {
        setenv("DISABLE_LSFG", "1", 1);

        if (previousInstanceLayers.has_value()) {
            const auto filtered = stripLayerName(
                *previousInstanceLayers, "VK_LAYER_LS_frame_generation");
            if (filtered.empty())
                unsetenv("VK_INSTANCE_LAYERS");
            else
                setenv("VK_INSTANCE_LAYERS", filtered.c_str(), 1);
        }

        if (previousLoaderLayersEnable.has_value()) {
            const auto filtered = stripLayerName(
                *previousLoaderLayersEnable, "VK_LAYER_LS_frame_generation");
            if (filtered.empty())
                unsetenv("VK_LOADER_LAYERS_ENABLE");
            else
                setenv("VK_LOADER_LAYERS_ENABLE", filtered.c_str(), 1);
        }
    }

    ~ScopedPrivateInstanceLayerSuppression() {
        restoreEnvironment("VK_LOADER_LAYERS_ENABLE", previousLoaderLayersEnable);
        restoreEnvironment("VK_INSTANCE_LAYERS", previousInstanceLayers);
        restoreEnvironment("DISABLE_LSFG", previousDisable);
    }

    ScopedPrivateInstanceLayerSuppression(
        const ScopedPrivateInstanceLayerSuppression&) = delete;
    ScopedPrivateInstanceLayerSuppression& operator=(
        const ScopedPrivateInstanceLayerSuppression&) = delete;

private:
    std::unique_lock<std::mutex> lock;
    std::optional<std::string> previousDisable;
    std::optional<std::string> previousInstanceLayers;
    std::optional<std::string> previousLoaderLayersEnable;
};

} // namespace

void LSFG::setInstanceProcAddr(PFN_vkGetInstanceProcAddr getInstanceProcAddr) {
    hostGetInstanceProcAddr.store(getInstanceProcAddr);
}

Instance::Instance() {
    // bbport: the host process loaded its Vulkan driver itself (no Android loader, no layers): the
    // framegen device is made through the same entry point, so it runs on that driver.
    const PFN_vkGetInstanceProcAddr hostGipa = hostGetInstanceProcAddr.load();
    if (hostGipa != nullptr) {
        volkInitializeCustom(hostGipa);
        if (vkCreateInstance == nullptr)
            throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
                "The host's Vulkan entry point has no vkCreateInstance");
    } else if (volkInitialize() != VK_SUCCESS)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "Failed to initialize Vulkan loader");

    uint32_t loaderVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion)
        vkEnumerateInstanceVersion(&loaderVersion);
    if (loaderVersion < VK_API_VERSION_1_1)
        throw LSFG::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER, "LSFG requires Vulkan 1.1 or newer");

    const uint32_t requestedVersion = loaderVersion >= VK_API_VERSION_1_2
        ? VK_API_VERSION_1_2
        : VK_API_VERSION_1_1;
    const VkApplicationInfo appInfo{
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lsfg-vk-base",
        .applicationVersion = VK_MAKE_VERSION(0, 0, 1),
        .pEngineName = "lsfg-vk-base",
        .engineVersion = VK_MAKE_VERSION(0, 0, 1),
        .apiVersion = requestedVersion,
    };
    const VkInstanceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance instanceHandle{};
    VkResult res = VK_ERROR_INITIALIZATION_FAILED;
    if (hostGipa != nullptr) {
        // No layer can be loaded (and setenv is not thread safe in a host with other threads).
        res = vkCreateInstance(&createInfo, nullptr, &instanceHandle);
    } else {
        ScopedPrivateInstanceLayerSuppression suppressRecursiveLsfgLayer;
        res = vkCreateInstance(&createInfo, nullptr, &instanceHandle);
    }
    if (res != VK_SUCCESS || instanceHandle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create capability-selected Vulkan instance");

    volkLoadInstance(instanceHandle);
    this->instance = std::shared_ptr<VkInstance>(
        new VkInstance(instanceHandle),
        [](VkInstance* instance) {
            if (instance != nullptr) {
                vkDestroyInstance(*instance, nullptr);
                delete instance;
            }
        });
}
