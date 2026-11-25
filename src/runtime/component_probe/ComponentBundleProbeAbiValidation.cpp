#include "runtime/component_probe/ComponentBundleProbeAbiValidation.h"

#include <algorithm>

namespace talas::runtime::component_probe
{
namespace
{
[[nodiscard]] constexpr ComponentBundleProbeAbiValidationResultV1 rejected(
    ComponentBundleProbeAbiValidationCodeV1 code,
    std::size_t kernelIndex = 0) noexcept
{
    return {code, kernelIndex};
}

[[nodiscard]] bool sameCompatibility(
    const TalasKernelAbiCompatibilityV1& candidate,
    const TalasKernelAbiCompatibilityV1& expected) noexcept
{
    return candidate.structSize == sizeof(TalasKernelAbiCompatibilityV1)
        && candidate.abiVersion == expected.abiVersion
        && candidate.pointerBits == expected.pointerBits
        && candidate.littleEndian == expected.littleEndian
        && candidate.doubleBits == expected.doubleBits
        && candidate.doubleIec559 == expected.doubleIec559
        && candidate.maximumAlignment == expected.maximumAlignment
        && candidate.reserved0 == 0 && expected.reserved0 == 0;
}

[[nodiscard]] bool hostBudgetValid(
    const ComponentBundleProbeHostObservationV1& observation) noexcept
{
    return !observation.violationObserved
        && observation.allocationCalls
            <= maximumComponentBundleProbeHostAllocationsV1
        && observation.allocationBytes
            <= maximumComponentBundleProbeHostAllocationBytesV1
        && observation.diagnosticCalls
            <= maximumComponentBundleProbeHostDiagnosticsV1
        && observation.diagnosticBytes
            <= maximumComponentBundleProbeHostDiagnosticBytesV1;
}
}

ComponentBundleProbeAbiValidationResultV1
validateComponentBundleProbeAbiV1(
    const ComponentBundleProbeCopiedModuleMetadataV1& module,
    const component_bundle::ComponentBundleManifest& manifest,
    const TalasKernelAbiCompatibilityV1& expectedCompatibility,
    const ComponentBundleProbeHostObservationV1& observation) noexcept
{
    if (!hostBudgetValid(observation))
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::HostBudgetExceeded);
    }
    if (module.structSize != sizeof(TalasKernelAbiModuleV1))
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::ModuleStructureInvalid);
    }
    if (module.descriptorVersion
        != TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::
                DescriptorVersionUnsupported);
    }
    if (module.reserved0 != 0 || module.reserved1 != 0
        || module.reserved2 != 0 || module.compatibility.reserved0 != 0)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::ReservedNonZero);
    }
    if (!sameCompatibility(module.compatibility, expectedCompatibility))
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::PlatformMismatch);
    }
    if (manifest.minimumEngineAbiVersion == 0
        || manifest.maximumEngineAbiVersion
            < manifest.minimumEngineAbiVersion
        || module.compatibility.abiVersion
            < manifest.minimumEngineAbiVersion
        || module.compatibility.abiVersion
            > manifest.maximumEngineAbiVersion)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::AbiRangeMismatch);
    }
    if (module.moduleId != manifest.bundleId)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::
                ManifestIdentityMismatch);
    }
    if (module.moduleVersion != manifest.semanticVersion)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::
                ManifestVersionMismatch);
    }
    if (manifest.minimumEngineApiVersion == 0
        || manifest.maximumEngineApiVersion
            < manifest.minimumEngineApiVersion
        || module.minimumApiVersion != manifest.minimumEngineApiVersion
        || module.maximumApiVersion != manifest.maximumEngineApiVersion
        || module.minimumApiVersion > TALAS_KERNEL_API_VERSION_V4
        || module.maximumApiVersion < TALAS_KERNEL_API_VERSION_V1)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::ApiRangeMismatch);
    }
    if (module.maximumApiVersion == TALAS_KERNEL_API_VERSION_V4
        && module.minimumApiVersion != TALAS_KERNEL_API_VERSION_V4)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::ApiRangeMismatch);
    }
    if (module.sampleFormats != TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::
                SampleFormatUnsupported);
    }
    if (module.trustClass != TALAS_KERNEL_ABI_TRUST_UNTRUSTED)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::TrustClassRejected);
    }
    if ((module.capabilityFlags
            & ~TALAS_KERNEL_ABI_CAPABILITY_KNOWN_MASK)
        != 0)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::CapabilityUnknown);
    }
    if (!module.hasUnload)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::CallbackMissing);
    }
    if (module.kernels.empty()
        || module.kernels.size()
            > component_bundle::maximumComponentBundleNodes)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::KernelBudgetExceeded);
    }
    if (module.kernels.size() != manifest.nodes.size())
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::KernelCountMismatch);
    }

    TalasKernelAbiCapabilityFlagsV1 combinedCapabilities = 0;
    for (std::size_t index = 0; index < module.kernels.size(); ++index)
    {
        const auto& kernel = module.kernels[index];
        if (kernel.structSize != sizeof(TalasKernelAbiDescriptorV1))
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::
                    KernelStructureInvalid,
                index);
        }
        if (kernel.descriptorVersion
            != TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1)
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::
                    DescriptorVersionUnsupported,
                index);
        }
        if (kernel.reserved0 != 0 || kernel.reserved1 != 0)
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::ReservedNonZero,
                index);
        }
        if (kernel.typeId.empty()
            || kernel.typeId.size()
                > component_bundle::maximumComponentBundleStableIdBytes)
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::
                    KernelIdentityInvalid,
                index);
        }
        if (!kernel.hasCreate || !kernel.hasPrepare || !kernel.hasReset
            || !kernel.hasProcess || !kernel.hasDestroy)
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::CallbackMissing,
                index);
        }
        if ((kernel.capabilityFlags
                & ~TALAS_KERNEL_ABI_CAPABILITY_KNOWN_MASK)
            != 0)
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::CapabilityUnknown,
                index);
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (module.kernels[previous].typeId == kernel.typeId)
            {
                return rejected(
                    ComponentBundleProbeAbiValidationCodeV1::KernelDuplicate,
                    index);
            }
        }
        const auto manifestNode = std::find_if(
            manifest.nodes.begin(),
            manifest.nodes.end(),
            [&](const auto& node) {
                return node.typeId == kernel.typeId;
            });
        if (manifestNode == manifest.nodes.end())
        {
            return rejected(
                ComponentBundleProbeAbiValidationCodeV1::KernelSetMismatch,
                index);
        }
        combinedCapabilities |= kernel.capabilityFlags;
    }
    if (module.capabilityFlags != combinedCapabilities)
    {
        return rejected(
            ComponentBundleProbeAbiValidationCodeV1::
                CapabilityAggregateMismatch);
    }
    return {ComponentBundleProbeAbiValidationCodeV1::Valid, 0};
}
}
