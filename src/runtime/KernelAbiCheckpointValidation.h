#pragma once

#include "talas/runtime/KernelAbi.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace talas::runtime::kernel_abi_checkpoint
{
enum class ValidationCode
{
    Valid, InvalidStructure, UnsupportedVersion, InvalidReservedField,
    InvalidMetadata, MissingCallback, UnknownNodeType, DuplicateIdentity,
};
struct ValidationResult
{
    ValidationCode code = ValidationCode::Valid;
    std::string path;
    std::vector<TalasKernelAbiCheckpointDescriptorV1> descriptors;
    [[nodiscard]] bool ok() const noexcept { return code == ValidationCode::Valid; }
};
namespace detail
{
[[nodiscard]] inline bool validUtf8(std::string_view value)
{
    std::size_t index = 0;
    while (index < value.size())
    {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        if (first <= 0x7fU)
        {
            if (first < 0x20U || first == 0x7fU)
                return false;
            ++index;
            continue;
        }
        if ((first & 0xe0U) == 0xc0U)
        {
            continuationCount = 1;
            codePoint = first & 0x1fU;
        }
        else if ((first & 0xf0U) == 0xe0U)
        {
            continuationCount = 2;
            codePoint = first & 0x0fU;
        }
        else if ((first & 0xf8U) == 0xf0U)
        {
            continuationCount = 3;
            codePoint = first & 0x07U;
        }
        else
        {
            return false;
        }
        if (index + continuationCount >= value.size())
            return false;
        for (std::size_t continuation = 0;
             continuation < continuationCount;
             ++continuation)
        {
            const auto byte = static_cast<unsigned char>(
                value[index + continuation + 1U]);
            if ((byte & 0xc0U) != 0x80U)
                return false;
            codePoint = (codePoint << 6U) | (byte & 0x3fU);
        }
        const auto minimum = continuationCount == 1 ? 0x80U
            : continuationCount == 2                  ? 0x800U
                                                       : 0x10000U;
        if (codePoint < minimum || codePoint > 0x10ffffU
            || (codePoint >= 0xd800U && codePoint <= 0xdfffU))
        {
            return false;
        }
        index += continuationCount + 1U;
    }
    return true;
}


inline std::optional<std::string_view> text(TalasKernelAbiStringViewV1 value, std::size_t maximumBytes)
{
    if (value.data == nullptr || value.byteCount == 0 || value.byteCount > maximumBytes
        || value.byteCount > std::numeric_limits<std::size_t>::max()) return std::nullopt;
    const std::string_view result(value.data, static_cast<std::size_t>(value.byteCount));
    return validUtf8(result) ? std::optional{result} : std::nullopt;
}
}

// Expected identities have already passed the caller's module/registry or
// manifest authority. No lifecycle callback runs here. Callback records are
// copied into type-indexed storage only after their complete validation.
inline ValidationResult validateAndCopy(const TalasKernelAbiCheckpointExtensionV1* extension,
    std::string_view moduleId, std::string_view moduleVersion,
    std::span<const std::string_view> types, std::size_t maximumIdentityBytes)
{
    const auto reject = [](ValidationCode code, std::string path) {
        return ValidationResult{code, std::move(path), {}};
    };
    ValidationResult result;
    result.descriptors.resize(types.size());
    if (extension == nullptr) return result;
    if (extension->structSize != sizeof(TalasKernelAbiCheckpointExtensionV1))
        return reject(ValidationCode::InvalidStructure, "checkpoints.structSize");
    if (extension->checkpointVersion != TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1)
        return reject(ValidationCode::UnsupportedVersion, "checkpoints.checkpointVersion");
    if (extension->reserved0 != 0 || extension->reserved1 != 0)
        return reject(ValidationCode::InvalidReservedField, "checkpoints.reserved");
    if (detail::text(extension->moduleId, maximumIdentityBytes) != moduleId
        || detail::text(extension->moduleVersion, maximumIdentityBytes) != moduleVersion)
        return reject(ValidationCode::InvalidMetadata, "checkpoints.identity");
    if (extension->kernels == nullptr || extension->kernelCount == 0 || extension->kernelCount > types.size())
        return reject(ValidationCode::InvalidStructure, "checkpoints.kernels");
    for (std::size_t index = 0; index < extension->kernelCount; ++index)
    {
        const auto& descriptor = extension->kernels[index];
        const auto path = "checkpoints.kernels[" + std::to_string(index) + "]";
        if (descriptor.structSize != sizeof(TalasKernelAbiCheckpointDescriptorV1))
            return reject(ValidationCode::InvalidStructure, path + ".structSize");
        if (descriptor.checkpointVersion != TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1)
            return reject(ValidationCode::UnsupportedVersion, path + ".checkpointVersion");
        if (descriptor.reserved0 != 0 || descriptor.reserved1 != 0)
            return reject(ValidationCode::InvalidReservedField, path + ".reserved");
        if (descriptor.capture == nullptr || descriptor.restore == nullptr || descriptor.destroyCheckpoint == nullptr)
            return reject(ValidationCode::MissingCallback, path + ".callbacks");
        const auto type = detail::text(descriptor.typeId, maximumIdentityBytes);
        if (!type) return reject(ValidationCode::InvalidMetadata, path + ".typeId");
        const auto found = std::ranges::find(types, *type);
        if (found == types.end()) return reject(ValidationCode::UnknownNodeType, path + ".typeId");
        auto& target = result.descriptors[static_cast<std::size_t>(found - types.begin())];
        if (target.capture != nullptr) return reject(ValidationCode::DuplicateIdentity, path + ".typeId");
        target = descriptor;
        target.typeId = {};
    }
    return result;
}
}
