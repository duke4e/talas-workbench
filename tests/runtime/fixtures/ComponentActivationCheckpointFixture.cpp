#include "talas/runtime/KernelAbi.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

// Compiled once per integration group or fixture mode; helpers that the
// current configuration does not use are expected.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#ifndef TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE
#define TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE 0
#endif

// Original Apache-2.0 stateful test image. Never packaged as a product module.
namespace
{
struct Snapshot;
struct Instance
{
    const TalasKernelAbiHostServicesV1* host = nullptr;
    Snapshot* snapshot = nullptr;
    double history = 0;
    int mode = 0;
    unsigned captures = 0;
};
struct Snapshot { Instance* owner; double history; };
unsigned liveInstances = 0;
unsigned createdInstances = 0;
unsigned unloadCount = 0;
__attribute__((destructor)) void verifyImageRetirement()
{
    if (liveInstances != 0 || (createdInstances != 0 && unloadCount != 1)) std::abort();
}

TalasKernelAbiStatusV1 create(const TalasKernelAbiHostServicesV1* host, void** output)
{
    *output = nullptr;
    void* storage = host->allocate(host->hostContext, sizeof(Instance), alignof(Instance));
    if (storage == nullptr) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    *output = new (storage) Instance{host};
    ++liveInstances;
    ++createdInstances;
    return TALAS_KERNEL_ABI_STATUS_OK;
}
TalasKernelAbiStatusV1 prepare(void* opaque, const TalasKernelAbiPrepareV1* value)
{
    if (value->parameterCount != 1 || value->parameters[0].kind != TALAS_KERNEL_ABI_PARAMETER_SCALAR)
        return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
    static_cast<Instance*>(opaque)->mode = static_cast<int>(value->parameters[0].scalarValue);
    return TALAS_KERNEL_ABI_STATUS_OK;
}
TalasKernelAbiStatusV1 reset(void* opaque, const TalasKernelAbiResetV1*)
{
    auto* instance = static_cast<Instance*>(opaque);
    if (instance->mode == 10) return TALAS_KERNEL_ABI_STATUS_FATAL;
    instance->history = 0;
    return TALAS_KERNEL_ABI_STATUS_OK;
}
TalasKernelAbiStatusV1 process(void* opaque, const TalasKernelAbiProcessV1* value)
{
    if (value->inputCount != 1 || value->outputCount != 1)
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    auto* instance = static_cast<Instance*>(opaque);
    for (std::uint64_t frame = 0; frame < value->frameCount; ++frame)
    {
        const auto input = value->inputs[0].kind == TALAS_KERNEL_ABI_SIGNAL_CONSTANT
            ? value->inputs[0].constantValue : value->inputs[0].samples[frame];
        instance->history = instance->history * .5 + input;
        value->outputs[0].samples[frame] = instance->history;
    }
    return TALAS_KERNEL_ABI_STATUS_OK;
}
void destroy(void* opaque, const TalasKernelAbiHostServicesV1* host)
{
    auto* instance = static_cast<Instance*>(opaque);
    if (instance->snapshot != nullptr || liveInstances != 1) std::abort();
    instance->~Instance();
    --liveInstances;
    host->deallocate(host->hostContext, instance, sizeof(Instance), alignof(Instance));
}
void unload(void*, const TalasKernelAbiHostServicesV1*)
{
    if (liveInstances != 0 || ++unloadCount != 1) std::abort();
}
TalasKernelAbiStatusV1 capture(void* opaque, const TalasKernelAbiHostServicesV1* host, void** output)
{
    auto* instance = static_cast<Instance*>(opaque);
    *output = nullptr;
    if (instance->snapshot != nullptr) return TALAS_KERNEL_ABI_STATUS_FATAL;
    ++instance->captures;
    if (instance->mode == 1 && instance->captures == 1) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    auto* storage = host->allocate(host->hostContext, sizeof(Snapshot), alignof(Snapshot));
    if (storage == nullptr) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    instance->snapshot = new (storage) Snapshot{instance, instance->history};
    *output = instance->snapshot;
    if (instance->mode == 2 && instance->captures == 1) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    if (instance->mode == 3) throw 3;
    if (instance->mode == 7)
    {
        std::array<void*, 64> extra{};
        for (auto& pointer : extra) pointer = host->allocate(host->hostContext, 8, 8);
        for (auto* pointer : extra) if (pointer != nullptr) host->deallocate(host->hostContext, pointer, 8, 8);
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    }
    if (instance->mode == 8) static_cast<void>(host->allocate(host->hostContext, 8, 8));
    return TALAS_KERNEL_ABI_STATUS_OK;
}
TalasKernelAbiStatusV1 restore(void* opaque, const void* payload)
{
    auto* instance = static_cast<Instance*>(opaque);
    const auto* snapshot = static_cast<const Snapshot*>(payload);
    if (snapshot->owner != instance || instance->snapshot != snapshot) std::abort();
    if (instance->mode == 4) return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    if (instance->mode == 5) static_cast<void>(instance->host->allocate(instance->host->hostContext, 8, 8));
    instance->history = snapshot->history;
    return TALAS_KERNEL_ABI_STATUS_OK;
}
void destroyCheckpoint(void* payload, const TalasKernelAbiHostServicesV1* host)
{
    auto* snapshot = static_cast<Snapshot*>(payload);
    auto* instance = snapshot->owner;
    if (instance->snapshot != snapshot) std::abort();
    if (instance->mode == 6)
        host->deallocate(host->hostContext, instance, sizeof(Instance), alignof(Instance));
    instance->snapshot = nullptr;
    snapshot->~Snapshot();
    host->deallocate(host->hostContext, snapshot, sizeof(Snapshot), alignof(Snapshot));
    if (instance->mode == 9) throw 9;
}
TalasKernelAbiCompatibilityV1 compatibility()
{
    TalasKernelAbiCompatibilityV1 value{};
    value.structSize = sizeof(value); value.abiVersion = TALAS_KERNEL_ABI_VERSION_V1;
    value.pointerBits = sizeof(void*) * 8; value.littleEndian = std::endian::native == std::endian::little;
    value.doubleBits = sizeof(double) * 8; value.doubleIec559 = std::numeric_limits<double>::is_iec559;
    value.maximumAlignment = alignof(std::max_align_t);
    return value;
}
const TalasKernelAbiModuleV1& module()
{
    static const TalasKernelAbiDescriptorV1 descriptor = {
        sizeof(TalasKernelAbiDescriptorV1), TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1,
        {"org.talas.activation.fixture.gain", sizeof("org.talas.activation.fixture.gain") - 1},
        TALAS_KERNEL_ABI_CAPABILITY_RESET | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL,
        create, prepare, reset, process, destroy, 0, 0,
    };
    static const TalasKernelAbiModuleV1 value = [] {
        TalasKernelAbiModuleV1 result{};
        result.structSize = sizeof(result); result.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        result.moduleId = {"org.talas.activation.fixture", sizeof("org.talas.activation.fixture") - 1}; result.moduleVersion = {"1.0.0", 5};
        result.minimumApiVersion = result.maximumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        result.compatibility = compatibility(); result.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
        result.trustClass = TALAS_KERNEL_ABI_TRUST_UNTRUSTED; result.capabilityFlags = descriptor.capabilityFlags;
        result.kernels = &descriptor; result.kernelCount = 1; result.unload = unload;
        return result;
    }();
    return value;
}
}
extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 talas_component_bundle_entry_v1(const TalasKernelAbiHostServicesV1*, const TalasKernelAbiModuleV1** output)
{
    *output = &module();
    return TALAS_KERNEL_ABI_STATUS_OK;
}
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE != 1
extern "C" __attribute__((visibility("default")))
TalasKernelAbiStatusV1 talas_component_checkpoint_entry_v1(const TalasKernelAbiCheckpointExtensionV1** output)
{
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 3
    throw 3;
#endif
    static const TalasKernelAbiCheckpointDescriptorV1 descriptor = {
        sizeof(TalasKernelAbiCheckpointDescriptorV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
        module().kernels[0].typeId, capture,
#if TALAS_COMPONENT_ACTIVATION_FIXTURE_MODE == 2
        nullptr,
#else
        restore,
#endif
        destroyCheckpoint, 0, 0,
    };
    static const TalasKernelAbiCheckpointExtensionV1 extension = {
        sizeof(TalasKernelAbiCheckpointExtensionV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
        module().moduleId, module().moduleVersion, &descriptor, 1, 0, 0,
    };
    *output = &extension;
    return TALAS_KERNEL_ABI_STATUS_OK;
}
#endif
