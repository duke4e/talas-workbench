#include "talas/runtime/KernelAbi.h"

#ifdef TALAS_COMPONENT_PROBE_FIXTURE_REQUIRES_UNLINKED_IMAGE
#include <dlfcn.h>
#include <sys/stat.h>
#endif

#include <bit>
#include <cstddef>
#include <limits>

// Original Talas Apache-2.0 test module. It is built only under
// TALAS_BUILD_TESTS and is never packaged into Workbench or either product helper.

namespace
{
TalasKernelAbiStatusV1 create(
    const TalasKernelAbiHostServicesV1*,
    void** instance)
{
    if (instance == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    *instance = reinterpret_cast<void*>(1);
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 prepare(
    void*,
    const TalasKernelAbiPrepareV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 reset(
    void*,
    const TalasKernelAbiResetV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 process(
    void*,
    const TalasKernelAbiProcessV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

void destroy(void*, const TalasKernelAbiHostServicesV1*)
{
}

void unload(void*, const TalasKernelAbiHostServicesV1*)
{
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
}

extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 talas_component_bundle_entry_v1(
    const TalasKernelAbiHostServicesV1*,
    const TalasKernelAbiModuleV1** output)
{
    if (output == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
#ifdef TALAS_COMPONENT_PROBE_FIXTURE_REQUIRES_UNLINKED_IMAGE
    // Module code must not run while the loader's private copy of this
    // image is still on disk.
    Dl_info image{};
    struct stat imageStatus{};
    if (dladdr(reinterpret_cast<const void*>(&talas_component_bundle_entry_v1), &image) == 0
        || image.dli_fname == nullptr
        || stat(image.dli_fname, &imageStatus) == 0)
    {
        return TALAS_KERNEL_ABI_STATUS_FATAL;
    }
#endif
    static const TalasKernelAbiDescriptorV1 kernel = [] {
        TalasKernelAbiDescriptorV1 value{};
        value.structSize = sizeof(value);
        value.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        value.typeId = {
            "org.talas.fixture.oscillator",
            sizeof("org.talas.fixture.oscillator") - 1U,
        };
        value.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET;
        value.create = create;
        value.prepare = prepare;
        value.reset = reset;
        value.process = process;
        value.destroy = destroy;
        return value;
    }();
    static const TalasKernelAbiModuleV1 module = [] {
        TalasKernelAbiModuleV1 value{};
        value.structSize = sizeof(value);
        value.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        value.moduleId = {
            "org.talas.fixture",
            sizeof("org.talas.fixture") - 1U,
        };
        value.moduleVersion = {"1.2.3", sizeof("1.2.3") - 1U};
        value.minimumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        value.maximumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        value.compatibility = compatibility();
        value.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
        value.trustClass = TALAS_KERNEL_ABI_TRUST_UNTRUSTED;
        value.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET;
        value.kernels = &kernel;
        value.kernelCount = 1;
        value.unload = unload;
        return value;
    }();
    *output = &module;
    return TALAS_KERNEL_ABI_STATUS_OK;
}
