#pragma once

#include "ahb_transport.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace LSFG {

struct DeviceIdentity {
    std::array<uint8_t, VK_UUID_SIZE> deviceUUID{};
    std::array<uint8_t, VK_UUID_SIZE> driverUUID{};

    [[nodiscard]] bool operator==(const DeviceIdentity& other) const noexcept {
        return deviceUUID == other.deviceUUID && driverUUID == other.driverUUID;
    }
};

struct AdaptiveFlowContextState {
    float requestedScale{0.0f};
    float activeScale{0.0f};
    uint32_t warmupRemaining{0};
    bool transitionPending{false};
};

// Metadata attached by the layer to one submitted frame-generation batch.
// Keeping this next to the GPU result makes asynchronous timing samples
// attributable without adding a CPU/GPU synchronization point.
struct AdaptiveFlowBatchMetadata {
    uint64_t sessionEpoch{0};
    uint64_t batchId{0};
    double frameBudgetMs{0.0};
    double predictedTotalLsfgMs{0.0};
    // A bounded prepared-graph handoff may keep generated output flowing while
    // the pending graph receives its three source-history writes.
    bool preserveOutputDuringTransition{false};
    // Hard scene/cadence discontinuities must overwrite every Alpha temporal
    // history slot before interpolation resumes, including fixed-flow fallback
    // contexts that do not own prepared Adaptive Flow graphs.
    bool resetTemporalHistory{false};
};

struct AdaptiveFlowGpuTiming {
    double mipmapsMs{0.0};
    double opticalFlowMs{0.0};
    double totalLsfgMs{0.0};
    double shadowMipmapsMs{0.0};
    double shadowAlphaMs{0.0};
    double shadowPreprocessMs{0.0};
    size_t generationCount{0};
    uint64_t sessionEpoch{0};
    uint64_t batchId{0};
    double frameBudgetMs{0.0};
    double predictedTotalLsfgMs{0.0};
    bool transitionActive{false};
    bool shadowPreprocessSubmitted{false};
    bool valid{false};
};

struct AndroidFrameSyncFds {
    std::vector<int> outputReadyFds;
    int batchCompleteFd{-1};
    uint64_t batchId{0};
    bool gpuDependenciesExported{false};
    bool hostWaitFallback{false};
};

struct FramegenSupportDecision {
    bool supported{false};
    std::string vulkanPath;
    uint32_t spirvTargetVersion{0};
    std::string synchronizationPath;
    AhbTransportMode ahbMode{AhbTransportMode::Unsupported};
    bool fp16{false};
    bool nullDescriptor{false};
    bool externalSyncFd{false};
    bool externalOpaqueFd{false};
    std::string rejectionReason;
};

struct BackendDiagnostics {
    FramegenSupportDecision supportDecision{};
    uint32_t apiVersion{VK_API_VERSION_1_0};
    uint32_t spirvTargetVersion{0};
    std::string vulkanPath;
    std::string synchronizationPath;
    std::string capabilitySummary;
    uint32_t driverVersion{0};
    VkDriverId driverId{static_cast<VkDriverId>(0)};
    std::string driverName;
    std::string driverInfo;
    DeviceIdentity identity{};
    bool ahbR16fStorage{false};
    bool ahbR16fTransferSrc{false};
    bool ahbR16fTransferDst{false};
    bool ahbR8Storage{false};
    bool ahbSampledInput{false};
    bool ahbStorageOutput{false};
    bool ahbTransferInput{false};
    bool ahbTransferOutput{false};
    AhbTransportMode ahbTransportMode{AhbTransportMode::Unsupported};
    // True only when the selected physical device advertises an OPAQUE_FD
    // external semaphore payload that is both exportable and importable. Android
    // uses this solely as an optional GPU-to-GPU AHB handoff optimization; the
    // established host-fence path remains the fallback when it is unavailable.
    bool externalSemaphoreOpaqueFd{false};
    bool externalSemaphoreSyncFd{false};
};

/// bbport: makes the framegen Vulkan instance through the host process's own vkGetInstanceProcAddr
/// (volkInitializeCustom) instead of the system loader. Call before the first initialize().
void setInstanceProcAddr(PFN_vkGetInstanceProcAddr getInstanceProcAddr);

/// bbport: whether the framegen device enables the shaderFloat16 feature when the GPU has it (the
/// half-precision shaders need it; default: yes). Call before the first initialize().
void setShaderFloat16Requested(bool requested);

inline constexpr uint64_t DEFAULT_DRIVER_WAIT_TIMEOUT_NS = 500'000'000ULL;

} // namespace LSFG
