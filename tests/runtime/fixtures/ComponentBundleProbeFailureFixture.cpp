#include "talas/runtime/KernelAbi.h"

#include <bit>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <unistd.h>

#ifndef TALAS_COMPONENT_PROBE_FAILURE_MODE
#error TALAS_COMPONENT_PROBE_FAILURE_MODE is required
#endif

// Original Talas Apache-2.0 disposable failure fixtures. Every target is
// built only under TALAS_BUILD_TESTS and is never packaged into Workbench.

namespace
{
#if TALAS_COMPONENT_PROBE_FAILURE_MODE != 1
#if TALAS_COMPONENT_PROBE_FAILURE_MODE == 3
struct ConstructorCrash final
{
    ConstructorCrash() { std::abort(); }
};
ConstructorCrash constructorCrash;
#endif

TalasKernelAbiStatusV1 create(
    const TalasKernelAbiHostServicesV1*,
    void** instance)
{
    if (instance == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
    *instance = reinterpret_cast<void*>(1);
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 prepare(void*, const TalasKernelAbiPrepareV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 reset(void*, const TalasKernelAbiResetV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 process(void*, const TalasKernelAbiProcessV1*)
{
    return TALAS_KERNEL_ABI_STATUS_OK;
}

void destroy(void*, const TalasKernelAbiHostServicesV1*)
{
}

void unload(void*, const TalasKernelAbiHostServicesV1*)
{
#if TALAS_COMPONENT_PROBE_FAILURE_MODE == 6
    std::abort();
#elif TALAS_COMPONENT_PROBE_FAILURE_MODE == 7
    while (true)
        pause();
#endif
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
#endif
}

#if TALAS_COMPONENT_PROBE_FAILURE_MODE == 1
extern "C" __attribute__((visibility("default")))
int talas_component_bundle_missing_entry_fixture_v1()
{
    return 1;
}
#else
extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 talas_component_bundle_entry_v1(
    const TalasKernelAbiHostServicesV1*,
    const TalasKernelAbiModuleV1** output)
{
#if TALAS_COMPONENT_PROBE_FAILURE_MODE == 4
    std::abort();
#elif TALAS_COMPONENT_PROBE_FAILURE_MODE == 5
    while (true)
        pause();
#endif
    if (output == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT;
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
#if TALAS_COMPONENT_PROBE_FAILURE_MODE == 2
        value.moduleId = {
            "org.talas.fixture-wrong",
            sizeof("org.talas.fixture-wrong") - 1U,
        };
#else
        value.moduleId = {
            "org.talas.fixture",
            sizeof("org.talas.fixture") - 1U,
        };
#endif
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
#endif
