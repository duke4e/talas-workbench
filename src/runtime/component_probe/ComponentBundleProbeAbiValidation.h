#pragma once

#include "talas/runtime/KernelAbi.h"
#include "runtime/component_bundle/ComponentBundleManifest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace talas::runtime::component_probe
{
inline constexpr std::string_view componentBundleProbeEntrySymbolV1 =
    "talas_component_bundle_entry_v1";
inline constexpr std::uint64_t maximumComponentBundleProbeHostAllocationsV1 =
    64;
inline constexpr std::uint64_t
    maximumComponentBundleProbeHostAllocationBytesV1 = 1024U * 1024U;
inline constexpr std::uint64_t maximumComponentBundleProbeHostDiagnosticsV1 =
    32;
inline constexpr std::uint64_t
    maximumComponentBundleProbeHostDiagnosticBytesV1 = 4096;

using ComponentBundleProbeEntryPointV1 = TalasKernelAbiStatusV1 (*)(
    const TalasKernelAbiHostServicesV1* host,
    const TalasKernelAbiModuleV1** module);

struct ComponentBundleProbeHostObservationV1
{
    std::uint64_t allocationCalls = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t diagnosticCalls = 0;
    std::uint64_t diagnosticBytes = 0;
    bool violationObserved = false;
};

struct ComponentBundleProbeCopiedKernelMetadataV1
{
    std::uint32_t structSize = 0;
    std::uint32_t descriptorVersion = 0;
    std::string typeId;
    TalasKernelAbiCapabilityFlagsV1 capabilityFlags = 0;
    bool hasCreate = false;
    bool hasPrepare = false;
    bool hasReset = false;
    bool hasProcess = false;
    bool hasDestroy = false;
    std::uint64_t reserved0 = 0;
    std::uint64_t reserved1 = 0;
};

struct ComponentBundleProbeCopiedModuleMetadataV1
{
    std::uint32_t structSize = 0;
    std::uint32_t descriptorVersion = 0;
    std::string moduleId;
    std::string moduleVersion;
    std::uint32_t minimumApiVersion = 0;
    std::uint32_t maximumApiVersion = 0;
    TalasKernelAbiCompatibilityV1 compatibility{};
    TalasKernelAbiSampleFormatMaskV1 sampleFormats = 0;
    TalasKernelAbiTrustClassV1 trustClass = 0;
    std::uint32_t reserved0 = 0;
    TalasKernelAbiCapabilityFlagsV1 capabilityFlags = 0;
    std::vector<ComponentBundleProbeCopiedKernelMetadataV1> kernels;
    bool hasUnload = false;
    std::uint64_t reserved1 = 0;
    std::uint64_t reserved2 = 0;
};

enum class ComponentBundleProbeAbiValidationCodeV1 : std::uint8_t
{
    Valid,
    HostBudgetExceeded,
    ModuleStructureInvalid,
    DescriptorVersionUnsupported,
    ReservedNonZero,
    PlatformMismatch,
    AbiRangeMismatch,
    ManifestIdentityMismatch,
    ManifestVersionMismatch,
    ApiRangeMismatch,
    SampleFormatUnsupported,
    TrustClassRejected,
    CapabilityUnknown,
    CallbackMissing,
    KernelBudgetExceeded,
    KernelCountMismatch,
    KernelStructureInvalid,
    KernelIdentityInvalid,
    KernelDuplicate,
    KernelSetMismatch,
    CapabilityAggregateMismatch,
};

struct ComponentBundleProbeAbiValidationResultV1
{
    ComponentBundleProbeAbiValidationCodeV1 code =
        ComponentBundleProbeAbiValidationCodeV1::ModuleStructureInvalid;
    std::size_t kernelIndex = 0;

    [[nodiscard]] constexpr bool ok() const noexcept
    {
        return code == ComponentBundleProbeAbiValidationCodeV1::Valid;
    }
};

[[nodiscard]] ComponentBundleProbeAbiValidationResultV1
validateComponentBundleProbeAbiV1(
    const ComponentBundleProbeCopiedModuleMetadataV1& module,
    const component_bundle::ComponentBundleManifest& manifest,
    const TalasKernelAbiCompatibilityV1& expectedCompatibility,
    const ComponentBundleProbeHostObservationV1& observation) noexcept;
}
