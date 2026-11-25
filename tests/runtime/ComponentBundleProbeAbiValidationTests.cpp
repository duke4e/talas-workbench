#include "runtime/component_probe/ComponentBundleProbeAbiValidation.h"

#include <bit>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace
{
namespace bundle = talas::runtime::component_bundle;
namespace probe = talas::runtime::component_probe;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

TalasKernelAbiCompatibilityV1 compatibility()
{
    TalasKernelAbiCompatibilityV1 value{};
    value.structSize = sizeof(value);
    value.abiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.pointerBits = sizeof(void*) * 8U;
    value.littleEndian =
        std::endian::native == std::endian::little ? 1U : 0U;
    value.doubleBits = sizeof(double) * 8U;
    value.doubleIec559 =
        std::numeric_limits<double>::is_iec559 ? 1U : 0U;
    value.maximumAlignment = alignof(std::max_align_t);
    return value;
}

bundle::ComponentBundleManifest manifest()
{
    bundle::ComponentBundleManifest value;
    value.bundleId = "org.talas.fixture";
    value.semanticVersion = "1.2.3";
    value.minimumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.maximumEngineAbiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.nodes.push_back({
        "org.talas.fixture.oscillator",
        1,
        {},
        {},
        {},
    });
    return value;
}

probe::ComponentBundleProbeCopiedModuleMetadataV1 metadata()
{
    probe::ComponentBundleProbeCopiedModuleMetadataV1 value;
    value.structSize = sizeof(TalasKernelAbiModuleV1);
    value.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
    value.moduleId = "org.talas.fixture";
    value.moduleVersion = "1.2.3";
    value.minimumApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.maximumApiVersion = TALAS_KERNEL_API_VERSION_V1;
    value.compatibility = compatibility();
    value.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
    value.trustClass = TALAS_KERNEL_ABI_TRUST_UNTRUSTED;
    value.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET;
    value.hasUnload = true;
    value.kernels.push_back({
        sizeof(TalasKernelAbiDescriptorV1),
        TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1,
        "org.talas.fixture.oscillator",
        TALAS_KERNEL_ABI_CAPABILITY_RESET,
        true,
        true,
        true,
        true,
        true,
        0,
        0,
    });
    return value;
}

void requireCode(
    const probe::ComponentBundleProbeCopiedModuleMetadataV1& value,
    probe::ComponentBundleProbeAbiValidationCodeV1 code,
    std::string_view message,
    probe::ComponentBundleProbeHostObservationV1 observation = {})
{
    const auto result = probe::validateComponentBundleProbeAbiV1(
        value,
        manifest(),
        compatibility(),
        observation);
    require(result.code == code, std::string(message));
}

void requireCodeForManifest(
    const probe::ComponentBundleProbeCopiedModuleMetadataV1& value,
    const bundle::ComponentBundleManifest& expectedManifest,
    probe::ComponentBundleProbeAbiValidationCodeV1 code,
    std::string_view message)
{
    const auto result = probe::validateComponentBundleProbeAbiV1(
        value,
        expectedManifest,
        compatibility(),
        {});
    require(result.code == code, std::string(message));
}

void testCleanMetadata()
{
    requireCode(
        metadata(),
        probe::ComponentBundleProbeAbiValidationCodeV1::Valid,
        "complete copied metadata should validate");
    static_assert(
        probe::componentBundleProbeEntrySymbolV1
            == std::string_view{"talas_component_bundle_entry_v1"});
    static_assert(
        std::is_pointer_v<probe::ComponentBundleProbeEntryPointV1>);

    auto v4Manifest = manifest();
    v4Manifest.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V4;
    v4Manifest.maximumEngineApiVersion = TALAS_KERNEL_API_VERSION_V4;
    auto v4Metadata = metadata();
    v4Metadata.minimumApiVersion = TALAS_KERNEL_API_VERSION_V4;
    v4Metadata.maximumApiVersion = TALAS_KERNEL_API_VERSION_V4;
    requireCodeForManifest(
        v4Metadata,
        v4Manifest,
        probe::ComponentBundleProbeAbiValidationCodeV1::Valid,
        "the closed v4 contract requires an exact signed v4 manifest range");

    v4Manifest.minimumEngineApiVersion = TALAS_KERNEL_API_VERSION_V3;
    v4Metadata.minimumApiVersion = TALAS_KERNEL_API_VERSION_V3;
    requireCodeForManifest(
        v4Metadata,
        v4Manifest,
        probe::ComponentBundleProbeAbiValidationCodeV1::ApiRangeMismatch,
        "an open-ended manifest range must not expose the closed v4 contract");
}

void testModuleBoundary()
{
    auto value = metadata();
    --value.structSize;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::ModuleStructureInvalid,
        "module layout must be exact");

    value = metadata();
    ++value.descriptorVersion;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::DescriptorVersionUnsupported,
        "unknown descriptor versions must reject");

    value = metadata();
    value.reserved0 = 1;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::ReservedNonZero,
        "reserved module fields must be zero");

    value = metadata();
    ++value.compatibility.pointerBits;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::PlatformMismatch,
        "platform compatibility must match exactly");

    value = metadata();
    value.compatibility.abiVersion = 2;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::PlatformMismatch,
        "a different ABI/platform tag must reject before manifest matching");

    auto wrongAbiManifest = manifest();
    wrongAbiManifest.minimumEngineAbiVersion = 2;
    wrongAbiManifest.maximumEngineAbiVersion = 2;
    requireCodeForManifest(
        metadata(),
        wrongAbiManifest,
        probe::ComponentBundleProbeAbiValidationCodeV1::AbiRangeMismatch,
        "the frozen ABI tag must lie inside the signed manifest range");

    value = metadata();
    value.moduleId = "org.talas.other";
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::ManifestIdentityMismatch,
        "module ID must equal the signed manifest");

    value = metadata();
    value.moduleVersion = "1.2.4";
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::ManifestVersionMismatch,
        "module version must equal the signed manifest");

    value = metadata();
    value.maximumApiVersion = 2;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::ApiRangeMismatch,
        "module and manifest API ranges must be identical");

    value = metadata();
    value.sampleFormats = 0;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::
            SampleFormatUnsupported,
        "probe ABI v1 requires exactly native f64 support");

    value = metadata();
    value.trustClass = TALAS_KERNEL_ABI_TRUST_BUILTIN;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::TrustClassRejected,
        "a user probe cannot claim built-in trust");

    value = metadata();
    value.hasUnload = false;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::CallbackMissing,
        "the module unload callback must be present without being called");

    value = metadata();
    value.capabilityFlags = 1ULL << 63U;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::CapabilityUnknown,
        "unknown module capability bits must reject");
}

void testKernelBoundary()
{
    auto value = metadata();
    value.kernels.front().structSize = 0;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::KernelStructureInvalid,
        "kernel layout must be exact");

    value = metadata();
    ++value.kernels.front().descriptorVersion;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::
            DescriptorVersionUnsupported,
        "unknown kernel descriptor versions must reject");

    value = metadata();
    value.kernels.front().reserved1 = 1;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::ReservedNonZero,
        "reserved kernel fields must be zero");

    value = metadata();
    value.kernels.front().hasProcess = false;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::CallbackMissing,
        "every lifecycle callback must be present without being called");

    value = metadata();
    value.kernels.front().typeId.assign(
        bundle::maximumComponentBundleStableIdBytes + 1U,
        'a');
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::
            KernelIdentityInvalid,
        "copied kernel identities must remain bounded");

    value = metadata();
    value.kernels.front().capabilityFlags = 1ULL << 63U;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::CapabilityUnknown,
        "unknown kernel capabilities must reject");

    value = metadata();
    value.capabilityFlags = 0;
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::CapabilityAggregateMismatch,
        "module flags must equal the kernel union");

    value = metadata();
    value.kernels.front().typeId = "org.talas.fixture.other";
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::KernelSetMismatch,
        "kernel IDs must exactly equal the signed manifest nodes");

    value = metadata();
    value.kernels.push_back(value.kernels.front());
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::KernelCountMismatch,
        "extra or duplicate kernels must reject");

    auto duplicateManifest = manifest();
    duplicateManifest.nodes.push_back({
        "org.talas.fixture.second",
        1,
        {},
        {},
        {},
    });
    requireCodeForManifest(
        value,
        duplicateManifest,
        probe::ComponentBundleProbeAbiValidationCodeV1::KernelDuplicate,
        "duplicate kernel identities must reject even at the expected count");

    value = metadata();
    value.kernels.clear();
    requireCode(
        value,
        probe::ComponentBundleProbeAbiValidationCodeV1::KernelBudgetExceeded,
        "a module without kernels must reject");
}

void testHostBudgets()
{
    auto observation = probe::ComponentBundleProbeHostObservationV1{};
    observation.allocationBytes =
        probe::maximumComponentBundleProbeHostAllocationBytesV1 + 1U;
    requireCode(
        metadata(),
        probe::ComponentBundleProbeAbiValidationCodeV1::HostBudgetExceeded,
        "host allocation budget overflow must reject",
        observation);

    observation = {};
    observation.allocationCalls =
        probe::maximumComponentBundleProbeHostAllocationsV1 + 1U;
    requireCode(
        metadata(),
        probe::ComponentBundleProbeAbiValidationCodeV1::HostBudgetExceeded,
        "host allocation call overflow must reject",
        observation);

    observation = {};
    observation.diagnosticBytes =
        probe::maximumComponentBundleProbeHostDiagnosticBytesV1 + 1U;
    requireCode(
        metadata(),
        probe::ComponentBundleProbeAbiValidationCodeV1::HostBudgetExceeded,
        "host diagnostic budget overflow must reject",
        observation);

    observation = {};
    observation.violationObserved = true;
    requireCode(
        metadata(),
        probe::ComponentBundleProbeAbiValidationCodeV1::HostBudgetExceeded,
        "a helper-observed service violation must dominate copied metadata",
        observation);
}
}

int main()
{
    testCleanMetadata();
    testModuleBoundary();
    testKernelBoundary();
    testHostBudgets();
    return 0;
}
