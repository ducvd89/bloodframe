#include <volk.h>
#include <vulkan/vulkan_core.h>

#include "core/capabilities.hpp"
#include "core/device.hpp"
#include "core/image.hpp"
#include "core/instance.hpp"
#include "common/exception.hpp"
#include "lsfg_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace LSFG::Core;

namespace {

// bloodframe: setShaderFloat16Requested(); the feature is enabled when requested and the GPU has it.
std::atomic<bool> shaderFloat16Requested{true};

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

void requireExtension(const std::vector<VkExtensionProperties>& available,
        std::vector<const char*>& enabled, const char* name) {
    if (!hasExtension(available, name))
        throw LSFG::vulkan_error(VK_ERROR_EXTENSION_NOT_PRESENT,
            std::string("Missing required device extension: ") + name);
    enabled.push_back(name);
}

LSFG::DeviceIdentity readIdentity(VkPhysicalDevice device) {
    VkPhysicalDeviceIDProperties idProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &idProperties,
    };
    vkGetPhysicalDeviceProperties2(device, &properties);

    LSFG::DeviceIdentity identity{};
    std::copy_n(idProperties.deviceUUID, VK_UUID_SIZE, identity.deviceUUID.begin());
    std::copy_n(idProperties.driverUUID, VK_UUID_SIZE, identity.driverUUID.begin());
    return identity;
}


bool probeAhbImageUsage(VkPhysicalDevice physicalDevice, VkFormat format,
        VkImageUsageFlags usage) {
#ifdef __ANDROID__
    if (vkGetPhysicalDeviceImageFormatProperties2 == nullptr)
        return false;
    const VkPhysicalDeviceExternalImageFormatInfo externalInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    const VkPhysicalDeviceImageFormatInfo2 imageInfo{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &externalInfo,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
    };
    VkExternalImageFormatProperties externalProperties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES,
    };
    VkImageFormatProperties2 imageProperties{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &externalProperties,
    };
    const auto result = vkGetPhysicalDeviceImageFormatProperties2(
        physicalDevice, &imageInfo, &imageProperties);
    return result == VK_SUCCESS
        && (externalProperties.externalMemoryProperties.externalMemoryFeatures
            & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) != 0;
#else
    (void)physicalDevice;
    (void)format;
    (void)usage;
    return true;
#endif
}

struct ExternalSemaphoreProbe {
    bool extensionPresent{false};
    bool queryAvailable{false};
    VkExternalSemaphoreFeatureFlags features{};
    VkExternalSemaphoreHandleTypeFlags compatibleHandleTypes{};
    VkExternalSemaphoreHandleTypeFlags exportFromImportedHandleTypes{};
    bool supported{false};
};

const char* externalSemaphoreHandleName(
        VkExternalSemaphoreHandleTypeFlagBits handleType) {
    switch (handleType) {
        case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT:
            return "opaque-fd";
        case VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT:
            return "sync-fd";
        default:
            return "other";
    }
}

ExternalSemaphoreProbe probeExternalSemaphoreSupport(VkPhysicalDevice physicalDevice,
        const std::vector<VkExtensionProperties>& availableExtensions,
        VkExternalSemaphoreHandleTypeFlagBits handleType) {
    ExternalSemaphoreProbe probe{};
#ifdef __ANDROID__
    probe.extensionPresent = hasExtension(
        availableExtensions, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    probe.queryAvailable = vkGetPhysicalDeviceExternalSemaphoreProperties != nullptr;
    if (!probe.extensionPresent || !probe.queryAvailable)
        return probe;

    const VkPhysicalDeviceExternalSemaphoreInfo info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        .handleType = handleType,
    };
    VkExternalSemaphoreProperties properties{
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(physicalDevice, &info, &properties);
    probe.features = properties.externalSemaphoreFeatures;
    probe.compatibleHandleTypes = properties.compatibleHandleTypes;
    probe.exportFromImportedHandleTypes = properties.exportFromImportedHandleTypes;

    constexpr VkExternalSemaphoreFeatureFlags required =
        VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT
        | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
    probe.supported =
        (probe.features & required) == required
        && (probe.compatibleHandleTypes & handleType) != 0;
#else
    (void)physicalDevice;
    (void)availableExtensions;
    (void)handleType;
    probe.extensionPresent = true;
    probe.queryAvailable = true;
    probe.supported = true;
#endif
    return probe;
}

void logFramegenExternalSemaphoreProbe(
        VkExternalSemaphoreHandleTypeFlagBits handleType,
        const ExternalSemaphoreProbe& probe) {
    std::cerr << "lsfg-vk: external-semaphore-probe scope=framegen"
              << " handle=" << externalSemaphoreHandleName(handleType)
              << " extension=" << (probe.extensionPresent ? 1 : 0)
              << " query=" << (probe.queryAvailable ? 1 : 0)
              << " features=0x" << std::hex
              << static_cast<uint32_t>(probe.features)
              << " compatible=0x"
              << static_cast<uint32_t>(probe.compatibleHandleTypes)
              << " export_from_imported=0x"
              << static_cast<uint32_t>(probe.exportFromImportedHandleTypes)
              << std::dec
              << " supported=" << (probe.supported ? 1 : 0)
              << '\n';
}

} // namespace

const Image& Device::getFallbackDescriptorImage() const {
    return *this->fallbackDescriptorImage;
}

void LSFG::setShaderFloat16Requested(bool requested) {
    shaderFloat16Requested.store(requested);
}

Device::Device(const Instance& instance, const LSFG::DeviceIdentity& requestedIdentity,
        VkFormat sharedFormat) {
    uint32_t deviceCount{};
    auto res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, nullptr);
    if (res != VK_SUCCESS || deviceCount == 0)
        throw LSFG::vulkan_error(res, "Failed to enumerate physical devices");
    std::vector<VkPhysicalDevice> devices(deviceCount);
    res = vkEnumeratePhysicalDevices(instance.handle(), &deviceCount, devices.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to get physical devices");

    std::optional<VkPhysicalDevice> selectedPhysicalDevice;
    VkPhysicalDeviceProperties properties{};
    LSFG::DeviceIdentity selectedIdentity{};
    for (const auto candidate : devices) {
        const auto candidateIdentity = readIdentity(candidate);
        if (candidateIdentity == requestedIdentity) {
            selectedPhysicalDevice = candidate;
            selectedIdentity = candidateIdentity;
            vkGetPhysicalDeviceProperties(candidate, &properties);
            break;
        }
    }
    if (!selectedPhysicalDevice)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED,
            "Could not find the exact physical-device/driver UUID pair selected by the game");
    if (properties.apiVersion < VK_API_VERSION_1_1)
        throw LSFG::vulkan_error(VK_ERROR_INCOMPATIBLE_DRIVER,
            "Selected physical device does not expose Vulkan 1.1");

    const auto physicalDevice = *selectedPhysicalDevice;
    uint32_t extensionCount{};
    res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr);
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to enumerate device extensions");
    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    res = vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr,
        &extensionCount, availableExtensions.data());
    if (res != VK_SUCCESS)
        throw LSFG::vulkan_error(res, "Failed to read device extensions");

    const bool api12 = properties.apiVersion >= VK_API_VERSION_1_2;
    const bool api13 = properties.apiVersion >= VK_API_VERSION_1_3;
    const bool hasSync2Ext = hasExtension(availableExtensions,
        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    const bool hasFloat16Ext = hasExtension(availableExtensions,
        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    const bool hasTimelineExt = hasExtension(availableExtensions,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    const bool hasSpirv14Ext = hasExtension(availableExtensions,
        VK_KHR_SPIRV_1_4_EXTENSION_NAME);
    const bool hasShaderFloatControlsExt = hasExtension(availableExtensions,
        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    const bool hasVulkanMemoryModelExt = hasExtension(availableExtensions,
        VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
    const bool hasDriverProperties = api12 || hasExtension(availableExtensions,
        VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);

    VkPhysicalDeviceDriverProperties driverProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
    };
    VkPhysicalDeviceIDProperties idProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        .pNext = hasDriverProperties ? &driverProperties : nullptr,
    };
    VkPhysicalDeviceProperties2 properties2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &idProperties,
    };
    vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);

    VkPhysicalDeviceSubgroupProperties subgroup{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
    };
    VkPhysicalDeviceProperties2 subgroupProperties{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &subgroup,
    };
    vkGetPhysicalDeviceProperties2(physicalDevice, &subgroupProperties);

    VkPhysicalDeviceSynchronization2Features sync2Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
    };
    VkPhysicalDeviceVulkan13Features features13Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features features12Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
    };
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR float16Probe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR,
    };
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timelineProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR,
    };
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR memoryModelProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR,
    };

    void* featureProbeHead = nullptr;
    if (api13) {
        features13Probe.pNext = featureProbeHead;
        featureProbeHead = &features13Probe;
    } else if (hasSync2Ext) {
        sync2Probe.pNext = featureProbeHead;
        featureProbeHead = &sync2Probe;
    }
    if (api12) {
        features12Probe.pNext = featureProbeHead;
        featureProbeHead = &features12Probe;
    } else {
        if (hasFloat16Ext) {
            float16Probe.pNext = featureProbeHead;
            featureProbeHead = &float16Probe;
        }
        if (hasTimelineExt) {
            timelineProbe.pNext = featureProbeHead;
            featureProbeHead = &timelineProbe;
        }
        if (hasVulkanMemoryModelExt) {
            memoryModelProbe.pNext = featureProbeHead;
            featureProbeHead = &memoryModelProbe;
        }
    }
    VkPhysicalDeviceFeatures2 featuresProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = featureProbeHead,
    };
    vkGetPhysicalDeviceFeatures2(physicalDevice, &featuresProbe);

    VulkanCapabilities caps{};
    caps.apiVersion = properties.apiVersion;
#ifdef __ANDROID__
    caps.androidHardwareBuffer = hasExtension(availableExtensions,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
#else
    caps.androidHardwareBuffer = true;
#endif
    caps.spirv14Extension = hasSpirv14Ext;
    caps.shaderFloatControlsExtension = hasShaderFloatControlsExt;
    caps.vulkanMemoryModelExtension = hasVulkanMemoryModelExt;
    caps.vulkanMemoryModel = api12
        ? features12Probe.vulkanMemoryModel == VK_TRUE
        : (hasVulkanMemoryModelExt && memoryModelProbe.vulkanMemoryModel == VK_TRUE);
    caps.vulkanMemoryModelDeviceScope = api12
        ? features12Probe.vulkanMemoryModelDeviceScope == VK_TRUE
        : (hasVulkanMemoryModelExt
            && memoryModelProbe.vulkanMemoryModelDeviceScope == VK_TRUE);
    caps.shaderStorageImageWriteWithoutFormat =
        featuresProbe.features.shaderStorageImageWriteWithoutFormat == VK_TRUE;
    caps.shaderStorageImageExtendedFormats =
        featuresProbe.features.shaderStorageImageExtendedFormats == VK_TRUE;
    caps.synchronization2Core = api13;
    caps.synchronization2Extension = hasSync2Ext;
    caps.synchronization2Feature = api13
        ? features13Probe.synchronization2 == VK_TRUE
        : (hasSync2Ext && sync2Probe.synchronization2 == VK_TRUE);
    caps.legacyPipelineBarrier = true;
    caps.timelineSemaphore = api12
        ? features12Probe.timelineSemaphore == VK_TRUE
        : (hasTimelineExt && timelineProbe.timelineSemaphore == VK_TRUE);
    caps.shaderFloat16 = api12
        ? features12Probe.shaderFloat16 == VK_TRUE
        : (hasFloat16Ext && float16Probe.shaderFloat16 == VK_TRUE);
    caps.subgroupStages = subgroup.supportedStages;
    caps.subgroupOperations = subgroup.supportedOperations;
    caps.subgroupSize = subgroup.subgroupSize;

    const SupportRequirements requirements{
#ifdef __ANDROID__
        .requireAhb = true,
#else
        .requireAhb = false,
#endif
        .requiredSubgroupStages = 0,
        .requiredSubgroupOperations = 0,
    };
    const auto decision = evaluateCapabilities(caps, requirements);
    if (!decision.supported)
        throw LSFG::vulkan_error(VK_ERROR_FEATURE_NOT_PRESENT,
            "LSFG capability check failed: " + decision.rejectionReason);

    this->diagnostics.apiVersion = properties.apiVersion;
    this->diagnostics.spirvTargetVersion =
        static_cast<uint32_t>(decision.spirvTarget);
    this->diagnostics.vulkanPath = vulkanPathName(decision.vulkanPath);
    this->diagnostics.synchronizationPath =
        synchronizationPathName(decision.synchronizationPath);
    this->diagnostics.capabilitySummary = "supported";
    this->diagnostics.supportDecision.supported = true;
    this->diagnostics.supportDecision.vulkanPath =
        this->diagnostics.vulkanPath;
    this->diagnostics.supportDecision.spirvTargetVersion =
        this->diagnostics.spirvTargetVersion;
    this->diagnostics.supportDecision.synchronizationPath =
        this->diagnostics.synchronizationPath;
    // bloodframe: the feature is enabled, not only there (setShaderFloat16Requested).
    const bool enableFloat16 = caps.shaderFloat16 && shaderFloat16Requested.load();
    this->diagnostics.supportDecision.fp16 = enableFloat16;
    this->diagnostics.driverVersion = properties.driverVersion;
    this->diagnostics.driverId = hasDriverProperties
        ? driverProperties.driverID
        : static_cast<VkDriverId>(0);
    this->diagnostics.identity = selectedIdentity;
    this->diagnostics.driverName = hasDriverProperties && driverProperties.driverName[0] != '\0'
        ? driverProperties.driverName
        : properties.deviceName;
    this->diagnostics.driverInfo = hasDriverProperties ? driverProperties.driverInfo : "";
#ifdef __ANDROID__
    this->diagnostics.ahbR16fStorage = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    this->diagnostics.ahbR16fTransferSrc = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    this->diagnostics.ahbR16fTransferDst = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    this->diagnostics.ahbR8Storage = probeAhbImageUsage(physicalDevice,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const bool sampledInput = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_SAMPLED_BIT);
    const bool storageOutput = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_STORAGE_BIT);
    const bool transferInput = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const bool transferOutput = probeAhbImageUsage(physicalDevice, sharedFormat,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    this->diagnostics.ahbSampledInput = sampledInput;
    this->diagnostics.ahbStorageOutput = storageOutput;
    this->diagnostics.ahbTransferInput = transferInput;
    this->diagnostics.ahbTransferOutput = transferOutput;
    this->diagnostics.ahbTransportMode = LSFG::selectAhbTransportMode(
        sampledInput, transferInput, storageOutput, transferOutput);
    this->diagnostics.supportDecision.ahbMode =
        this->diagnostics.ahbTransportMode;
    if (this->diagnostics.ahbTransportMode == LSFG::AhbTransportMode::Unsupported) {
        this->diagnostics.supportDecision.supported = false;
        this->diagnostics.supportDecision.rejectionReason =
            "no supported directional AHardwareBuffer transport for LSFG format";
    }
    const auto opaqueFdSemaphoreProbe = probeExternalSemaphoreSupport(
        physicalDevice, availableExtensions,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT);
    const auto syncFdSemaphoreProbe = probeExternalSemaphoreSupport(
        physicalDevice, availableExtensions,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT);
    logFramegenExternalSemaphoreProbe(
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, opaqueFdSemaphoreProbe);
    logFramegenExternalSemaphoreProbe(
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, syncFdSemaphoreProbe);
    this->diagnostics.externalSemaphoreOpaqueFd = opaqueFdSemaphoreProbe.supported;
    this->diagnostics.externalSemaphoreSyncFd = syncFdSemaphoreProbe.supported;
    this->diagnostics.supportDecision.externalOpaqueFd =
        opaqueFdSemaphoreProbe.supported;
    this->diagnostics.supportDecision.externalSyncFd =
        syncFdSemaphoreProbe.supported;
#else
    this->diagnostics.ahbTransportMode = LSFG::AhbTransportMode::DirectStorage;
    this->diagnostics.externalSemaphoreOpaqueFd = true;
    this->diagnostics.externalSemaphoreSyncFd = false;
    this->diagnostics.supportDecision.ahbMode =
        LSFG::AhbTransportMode::DirectStorage;
    this->diagnostics.supportDecision.externalOpaqueFd = true;
#endif

    std::cerr << "lsfg-vk: backend driver=\"" << this->diagnostics.driverName
              << "\" driver_id=" << static_cast<uint32_t>(this->diagnostics.driverId)
              << " ahb_mode=" << LSFG::ahbTransportModeName(this->diagnostics.ahbTransportMode)
              << " externalSemaphoreOpaqueFd="
              << (this->diagnostics.externalSemaphoreOpaqueFd ? 1 : 0)
              << " externalSemaphoreSyncFd="
              << (this->diagnostics.externalSemaphoreSyncFd ? 1 : 0)
              << " vulkan_path=" << vulkanPathName(decision.vulkanPath)
              << " spirv_target=" << spirvTargetName(decision.spirvTarget)
              << " sync=" << synchronizationPathName(decision.synchronizationPath) << '\n';

    uint32_t familyCount{};
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> queueFamilies(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, queueFamilies.data());
    std::optional<uint32_t> computeFamilyIdx;
    for (uint32_t i = 0; i < familyCount; ++i) {
        if (queueFamilies[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            computeFamilyIdx = i;
            break;
        }
    }
    if (!computeFamilyIdx)
        throw LSFG::vulkan_error(VK_ERROR_INITIALIZATION_FAILED, "No compute queue family found");

    std::vector<const char*> enabledExtensions;
#ifdef __ANDROID__
    requireExtension(availableExtensions, enabledExtensions,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    if (this->diagnostics.externalSemaphoreOpaqueFd
            || this->diagnostics.externalSemaphoreSyncFd)
        enabledExtensions.push_back(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#else
    requireExtension(availableExtensions, enabledExtensions,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    requireExtension(availableExtensions, enabledExtensions,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
#endif
    if (decision.synchronizationPath == SynchronizationPath::KhrSync2)
        enabledExtensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (!api12 && enableFloat16)
        enabledExtensions.push_back(VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME);
    if (decision.vulkanPath == VulkanPath::Vulkan11Extensions) {
        enabledExtensions.push_back(VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
        enabledExtensions.push_back(VK_KHR_SPIRV_1_4_EXTENSION_NAME);
        enabledExtensions.push_back(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
    }

    const bool hasRobustness2 = hasExtension(availableExtensions,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    VkPhysicalDeviceRobustness2FeaturesEXT robustnessProbe{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
    };
    if (hasRobustness2) {
        VkPhysicalDeviceFeatures2 probe{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &robustnessProbe,
        };
        vkGetPhysicalDeviceFeatures2(physicalDevice, &probe);
        if (robustnessProbe.nullDescriptor == VK_TRUE)
            enabledExtensions.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    }
    const bool enableNullDescriptor = hasRobustness2 && robustnessProbe.nullDescriptor == VK_TRUE;
    caps.nullDescriptor = enableNullDescriptor;

    VkPhysicalDeviceFeatures enabledCore{};
    enabledCore.shaderStorageImageExtendedFormats =
        featuresProbe.features.shaderStorageImageExtendedFormats;
    enabledCore.shaderStorageImageReadWithoutFormat =
        featuresProbe.features.shaderStorageImageReadWithoutFormat;
    enabledCore.shaderStorageImageWriteWithoutFormat =
        featuresProbe.features.shaderStorageImageWriteWithoutFormat;
    enabledCore.shaderInt16 = featuresProbe.features.shaderInt16;

    VkPhysicalDeviceRobustness2FeaturesEXT robustnessEnable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .nullDescriptor = enableNullDescriptor ? VK_TRUE : VK_FALSE,
    };
    VkPhysicalDeviceSynchronization2Features sync2Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES,
        .synchronization2 = decision.synchronizationPath == SynchronizationPath::KhrSync2,
    };
    VkPhysicalDeviceVulkan13Features features13Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = decision.synchronizationPath == SynchronizationPath::Core13Sync2,
    };
    VkPhysicalDeviceVulkan12Features features12Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .shaderFloat16 = api12 && enableFloat16 ? VK_TRUE : VK_FALSE,
        .timelineSemaphore = VK_FALSE,
        .vulkanMemoryModel = api12 ? features12Probe.vulkanMemoryModel : VK_FALSE,
    };
    VkPhysicalDeviceShaderFloat16Int8FeaturesKHR float16Enable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES_KHR,
        .shaderFloat16 = !api12 && enableFloat16 ? VK_TRUE : VK_FALSE,
    };
    VkPhysicalDeviceVulkanMemoryModelFeaturesKHR memoryModelEnable{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES_KHR,
        .vulkanMemoryModel =
            decision.vulkanPath == VulkanPath::Vulkan11Extensions ? VK_TRUE : VK_FALSE,
        .vulkanMemoryModelDeviceScope =
            decision.vulkanPath == VulkanPath::Vulkan11Extensions
                && caps.vulkanMemoryModelDeviceScope ? VK_TRUE : VK_FALSE,
    };

    void* enableHead = enableNullDescriptor ? &robustnessEnable : nullptr;
    if (decision.synchronizationPath == SynchronizationPath::Core13Sync2) {
        features13Enable.pNext = enableHead;
        enableHead = &features13Enable;
    } else if (decision.synchronizationPath == SynchronizationPath::KhrSync2) {
        sync2Enable.pNext = enableHead;
        enableHead = &sync2Enable;
    }
    if (api12) {
        features12Enable.pNext = enableHead;
        enableHead = &features12Enable;
    } else {
        if (enableFloat16) {
            float16Enable.pNext = enableHead;
            enableHead = &float16Enable;
        }
        if (decision.vulkanPath == VulkanPath::Vulkan11Extensions) {
            memoryModelEnable.pNext = enableHead;
            enableHead = &memoryModelEnable;
        }
    }

    VkPhysicalDeviceFeatures2 enabledFeatures2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = enableHead,
        .features = enabledCore,
    };
    const float priority = 1.0F;
    const VkDeviceQueueCreateInfo queueInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = *computeFamilyIdx,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    const VkDeviceCreateInfo createInfo{
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &enabledFeatures2,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &queueInfo,
        .enabledExtensionCount = static_cast<uint32_t>(enabledExtensions.size()),
        .ppEnabledExtensionNames = enabledExtensions.data(),
    };

    VkDevice handle{};
    res = vkCreateDevice(physicalDevice, &createInfo, nullptr, &handle);
    if (res != VK_SUCCESS || handle == VK_NULL_HANDLE)
        throw LSFG::vulkan_error(res, "Failed to create capability-selected logical device");
    volkLoadDevice(handle);

    if (decision.synchronizationPath == SynchronizationPath::KhrSync2
            && vkCmdPipelineBarrier2 == nullptr && vkCmdPipelineBarrier2KHR != nullptr) {
        vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(vkCmdPipelineBarrier2KHR);
    }
    if (decision.synchronizationPath == SynchronizationPath::LegacyPipelineBarrier)
        vkCmdPipelineBarrier2 = nullptr;

    VkQueue queue{};
    vkGetDeviceQueue(handle, *computeFamilyIdx, 0, &queue);
    this->computeQueue = queue;
    this->computeFamilyIdx = *computeFamilyIdx;
    this->physicalDevice = physicalDevice;
    this->nullDescriptorSupported = enableNullDescriptor;
    this->diagnostics.supportDecision.nullDescriptor = enableNullDescriptor;
    this->device = std::shared_ptr<VkDevice>(
        new VkDevice(handle), [](VkDevice* device) {
            if (device != nullptr) {
                vkDestroyDevice(*device, nullptr);
                delete device;
            }
        });

    if (!this->nullDescriptorSupported) {
        this->fallbackDescriptorImage = std::make_shared<Core::Image>(*this,
            VkExtent2D{1, 1}, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    }
}
