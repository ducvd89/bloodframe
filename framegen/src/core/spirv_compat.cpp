#include "core/spirv_compat.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace LSFG::Core {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr size_t kHeaderWords = 5u;
constexpr uint32_t kWordCountShift = 16u;
constexpr uint32_t kOpcodeMask = 0xffffu;
constexpr uint32_t kSpirv10 = 0x00010000u;
constexpr uint32_t kSpirv14 = 0x00010400u;
constexpr uint32_t kSpirv15 = 0x00010500u;
constexpr uint32_t kSpirv16 = 0x00010600u;

constexpr uint32_t kOpExtension = 10u;
constexpr uint32_t kOpExtInstImport = 11u;
constexpr uint32_t kOpMemoryModel = 14u;
constexpr uint32_t kOpCapability = 17u;
constexpr uint32_t kOpFunction = 54u;
constexpr uint32_t kOpTerminateInvocation = 4416u;
constexpr uint32_t kOpSDot = 4450u;
constexpr uint32_t kOpUDot = 4451u;
constexpr uint32_t kOpSUDot = 4452u;
constexpr uint32_t kOpSDotAccSat = 4453u;
constexpr uint32_t kOpUDotAccSat = 4454u;
constexpr uint32_t kOpSUDotAccSat = 4455u;
constexpr uint32_t kOpDemoteToHelperInvocation = 5380u;

constexpr uint32_t kCapStorageBuffer8BitAccess = 4448u;
constexpr uint32_t kCapUniformAndStorageBuffer8BitAccess = 4449u;
constexpr uint32_t kCapStoragePushConstant8 = 4450u;
constexpr uint32_t kCapShaderNonUniform = 5301u;
constexpr uint32_t kCapRuntimeDescriptorArray = 5302u;
constexpr uint32_t kCapInputAttachmentArrayDynamicIndexing = 5303u;
constexpr uint32_t kCapUniformTexelBufferArrayDynamicIndexing = 5304u;
constexpr uint32_t kCapStorageTexelBufferArrayDynamicIndexing = 5305u;
constexpr uint32_t kCapUniformBufferArrayNonUniformIndexing = 5306u;
constexpr uint32_t kCapSampledImageArrayNonUniformIndexing = 5307u;
constexpr uint32_t kCapStorageBufferArrayNonUniformIndexing = 5308u;
constexpr uint32_t kCapStorageImageArrayNonUniformIndexing = 5309u;
constexpr uint32_t kCapInputAttachmentArrayNonUniformIndexing = 5310u;
constexpr uint32_t kCapUniformTexelBufferArrayNonUniformIndexing = 5311u;
constexpr uint32_t kCapStorageTexelBufferArrayNonUniformIndexing = 5312u;
constexpr uint32_t kCapVulkanMemoryModel = 5345u;
constexpr uint32_t kCapVulkanMemoryModelDeviceScope = 5346u;
constexpr uint32_t kCapPhysicalStorageBufferAddresses = 5347u;
constexpr uint32_t kCapDemoteToHelperInvocation = 5379u;
constexpr uint32_t kCapDotProductInputAll = 6016u;
constexpr uint32_t kCapDotProductInput4x8Bit = 6017u;
constexpr uint32_t kCapDotProductInput4x8BitPacked = 6018u;
constexpr uint32_t kCapDotProduct = 6019u;

bool validTarget(uint32_t target) {
    return target == kSpirv14 || target == kSpirv15 || target == kSpirv16;
}

bool isSpirv16OnlyCapability(uint32_t capability) {
    return capability == kCapDemoteToHelperInvocation
        || (capability >= kCapDotProductInputAll && capability <= kCapDotProduct);
}

bool isSpirv15PromotedCapabilityWithoutCompatPath(uint32_t capability) {
    switch (capability) {
    case kCapStorageBuffer8BitAccess:
    case kCapUniformAndStorageBuffer8BitAccess:
    case kCapStoragePushConstant8:
    case kCapShaderNonUniform:
    case kCapRuntimeDescriptorArray:
    case kCapInputAttachmentArrayDynamicIndexing:
    case kCapUniformTexelBufferArrayDynamicIndexing:
    case kCapStorageTexelBufferArrayDynamicIndexing:
    case kCapUniformBufferArrayNonUniformIndexing:
    case kCapSampledImageArrayNonUniformIndexing:
    case kCapStorageBufferArrayNonUniformIndexing:
    case kCapStorageImageArrayNonUniformIndexing:
    case kCapInputAttachmentArrayNonUniformIndexing:
    case kCapUniformTexelBufferArrayNonUniformIndexing:
    case kCapStorageTexelBufferArrayNonUniformIndexing:
    case kCapPhysicalStorageBufferAddresses:
        return true;
    default:
        return false;
    }
}

bool isSpirv16OnlyOpcode(uint32_t opcode) {
    return opcode == kOpTerminateInvocation
        || opcode == kOpDemoteToHelperInvocation
        || (opcode >= kOpSDot && opcode <= kOpSUDotAccSat);
}

bool isNonSemanticImport(const std::vector<uint32_t>& words, size_t offset, uint32_t count) {
    if (count < 3)
        return false;
    const auto* value = reinterpret_cast<const char*>(&words[offset + 2]);
    const size_t bytes = static_cast<size_t>(count - 2) * sizeof(uint32_t);
    constexpr char prefix[] = "NonSemantic.";
    return bytes >= sizeof(prefix) - 1
        && std::strncmp(value, prefix, sizeof(prefix) - 1) == 0;
}

std::vector<uint32_t> bytesToWords(const std::vector<uint8_t>& bytes) {
    std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
    if (!words.empty())
        std::memcpy(words.data(), bytes.data(), bytes.size());
    return words;
}

std::vector<uint8_t> wordsToBytes(const std::vector<uint32_t>& words) {
    std::vector<uint8_t> bytes(words.size() * sizeof(uint32_t));
    if (!bytes.empty())
        std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

std::vector<uint32_t> encodeExtension(const char* name) {
    const size_t length = std::strlen(name) + 1;
    const size_t stringWords = (length + 3) / 4;
    std::vector<uint32_t> instruction(1 + stringWords, 0u);
    instruction[0] =
        (static_cast<uint32_t>(1 + stringWords) << kWordCountShift) | kOpExtension;
    std::memcpy(&instruction[1], name, length);
    return instruction;
}

bool hasExtension(const std::vector<uint32_t>& words, const char* expected) {
    size_t offset = kHeaderWords;
    while (offset < words.size()) {
        const uint32_t count = words[offset] >> kWordCountShift;
        const uint32_t opcode = words[offset] & kOpcodeMask;
        if (count == 0 || offset + count > words.size())
            return false;
        if (opcode == kOpMemoryModel || opcode == kOpFunction)
            break;
        if (opcode == kOpExtension && count >= 2) {
            const auto* value =
                reinterpret_cast<const char*>(&words[offset + 1]);
            const size_t bytes = static_cast<size_t>(count - 1) * sizeof(uint32_t);
            if (std::strncmp(value, expected, bytes) == 0)
                return true;
        }
        offset += count;
    }
    return false;
}

} // namespace

SpirvCompatibilityResult prepareSpirvForTarget(
        const std::vector<uint8_t>& source, uint32_t targetVersion) {
    SpirvCompatibilityResult result{};
    result.targetVersion = targetVersion;

    if (!validTarget(targetVersion)) {
        result.rejectionReason = "unsupported SPIR-V target";
        return result;
    }
    if (source.size() < kHeaderWords * sizeof(uint32_t)
            || source.size() % sizeof(uint32_t) != 0) {
        result.rejectionReason = "SPIR-V module has invalid byte length";
        return result;
    }

    auto words = bytesToWords(source);
    if (words[0] != kSpirvMagic) {
        result.rejectionReason = "SPIR-V module has invalid magic";
        return result;
    }
    result.sourceVersion = words[1];
    // bbport: modules older than the target (the half-precision shaders of the Lossless.dll are 1.0)
    // need no change: every Vulkan 1.1+ device takes 1.0 up to its own target.
    if (result.sourceVersion < kSpirv10 || result.sourceVersion > kSpirv16) {
        result.rejectionReason =
            "SPIR-V module version is outside audited 1.0-1.6 range";
        return result;
    }

    bool needsMemoryModelExtension = false;
    size_t insertAt = kHeaderWords;
    size_t offset = kHeaderWords;
    while (offset < words.size()) {
        const uint32_t count = words[offset] >> kWordCountShift;
        const uint32_t opcode = words[offset] & kOpcodeMask;
        if (count == 0 || offset + count > words.size()) {
            result.rejectionReason =
                "SPIR-V module contains malformed instruction";
            return result;
        }

        if (opcode == kOpCapability && count >= 2) {
            const uint32_t capability = words[offset + 1];
            result.capabilities.push_back(capability);
            if (capability == kCapVulkanMemoryModel
                    || capability == kCapVulkanMemoryModelDeviceScope) {
                needsMemoryModelExtension = true;
            }
            if (targetVersion < kSpirv16
                    && isSpirv16OnlyCapability(capability)) {
                result.rejectionReason =
                    "SPIR-V module uses a SPIR-V 1.6-only capability without an audited compatibility path";
                return result;
            }
            if (targetVersion < kSpirv15
                    && isSpirv15PromotedCapabilityWithoutCompatPath(capability)) {
                result.rejectionReason =
                    "SPIR-V module uses a SPIR-V 1.5 capability whose extension path is not enabled";
                return result;
            }
            insertAt = offset + count;
        } else if (opcode == kOpExtension) {
            insertAt = offset + count;
        } else if (targetVersion < kSpirv16 && isSpirv16OnlyOpcode(opcode)) {
            result.rejectionReason =
                "SPIR-V module uses a SPIR-V 1.6-only instruction without an audited compatibility path";
            return result;
        } else if (targetVersion < kSpirv16 && opcode == kOpExtInstImport
                && isNonSemanticImport(words, offset, count)) {
            result.rejectionReason =
                "SPIR-V module imports non-semantic instructions on an unaudited compatibility path";
            return result;
        }

        offset += count;
    }

    std::sort(result.capabilities.begin(), result.capabilities.end());
    result.capabilities.erase(
        std::unique(result.capabilities.begin(), result.capabilities.end()),
        result.capabilities.end());

    if (result.sourceVersion > targetVersion) {
        if (targetVersion < kSpirv15 && needsMemoryModelExtension
                && !hasExtension(words, "SPV_KHR_vulkan_memory_model")) {
            const auto extension =
                encodeExtension("SPV_KHR_vulkan_memory_model");
            words.insert(
                words.begin() + static_cast<std::ptrdiff_t>(insertAt),
                extension.begin(), extension.end());
        }
        words[1] = targetVersion;
        result.lowered = true;
    }

    result.code = wordsToBytes(words);
    result.supported = true;
    return result;
}

} // namespace LSFG::Core
