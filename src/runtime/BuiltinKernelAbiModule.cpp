#include "talas/runtime/BuiltinKernelAbiModule.h"

#include "talas/runtime/KernelAbiHost.h"
#include "talas/runtime/KernelRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>

namespace talas::runtime
{
namespace
{
struct OscillatorAbiInstance
{
    OscillatorKernel kernel;
};

struct FilterAbiInstance
{
    FilterKernel kernel;
    const TalasKernelAbiHostServicesV1* host = nullptr;
    double* outputScratch = nullptr;
    std::size_t outputCapacity = 0;
};

struct AdsrAbiInstance
{
    AdsrKernel kernel;
    const TalasKernelAbiHostServicesV1* host = nullptr;
    AdsrGateEvent* eventScratch = nullptr;
    std::size_t eventCapacity = 0;
    double attackTime = 0.01;
    double decayTime = 0.1;
    double sustainLevel = 0.7;
    double releaseTime = 0.25;
    double scale = 1.0;
    double legato = 0.0;
};

// Typed kernel values contain the complete processing history. ABI scratch is
// overwritten before use and remains owned by the live instance; fixed prepared
// parameters never change during processing. No opaque instance memory is copied.
template <typename Instance>
using CheckpointKernel = decltype(Instance::kernel);

template <typename Instance>
TalasKernelAbiStatusV1 captureCheckpoint(void* opaque,
    const TalasKernelAbiHostServicesV1* host, void** checkpoint) noexcept
{
    using Kernel = CheckpointKernel<Instance>;
    static_assert(std::is_nothrow_copy_constructible_v<Kernel>);
    *checkpoint = nullptr;
    void* storage = nullptr;
    try
    {
        storage = host->allocate(host->hostContext, sizeof(Kernel), alignof(Kernel));
    }
    catch (...)
    {
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    }
    if (storage == nullptr) return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    *checkpoint = new (storage) Kernel(static_cast<Instance*>(opaque)->kernel);
    return TALAS_KERNEL_ABI_STATUS_OK;
}

template <typename Instance>
TalasKernelAbiStatusV1 restoreCheckpoint(void* opaque, const void* checkpoint) noexcept
{
    using Kernel = CheckpointKernel<Instance>;
    static_assert(std::is_nothrow_copy_assignable_v<Kernel>);
    static_cast<Instance*>(opaque)->kernel = *static_cast<const Kernel*>(checkpoint);
    return TALAS_KERNEL_ABI_STATUS_OK;
}

template <typename Instance>
void destroyCheckpoint(void* checkpoint, const TalasKernelAbiHostServicesV1* host)
{
    using Kernel = CheckpointKernel<Instance>;
    static_cast<Kernel*>(checkpoint)->~Kernel();
    host->deallocate(host->hostContext, checkpoint, sizeof(Kernel), alignof(Kernel));
}

[[nodiscard]] std::string_view text(TalasKernelAbiStringViewV1 view) noexcept
{
    if (view.byteCount == 0)
        return {};
    return {view.data, static_cast<std::size_t>(view.byteCount)};
}

[[nodiscard]] TalasKernelAbiStatusV1 status(
    KernelProcessStatus kernelStatus) noexcept
{
    switch (kernelStatus)
    {
    case KernelProcessStatus::Ok:
        return TALAS_KERNEL_ABI_STATUS_OK;
    case KernelProcessStatus::NotPrepared:
        return TALAS_KERNEL_ABI_STATUS_NOT_PREPARED;
    case KernelProcessStatus::InvalidBlockLayout:
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    case KernelProcessStatus::InvalidConfiguration:
        return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
    case KernelProcessStatus::InvalidValue:
        return TALAS_KERNEL_ABI_STATUS_INVALID_VALUE;
    }
    return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
}

[[nodiscard]] KernelSignal signal(
    const TalasKernelAbiSignalV1& abiSignal) noexcept
{
    if (abiSignal.kind == TALAS_KERNEL_ABI_SIGNAL_PER_FRAME)
    {
        return KernelSignal::sampleValues({
            abiSignal.samples,
            static_cast<std::size_t>(abiSignal.sampleCount),
        });
    }
    return KernelSignal::constantValue(abiSignal.constantValue);
}

template <typename Instance>
[[nodiscard]] TalasKernelAbiStatusV1 createInstance(
    const TalasKernelAbiHostServicesV1* host,
    void** instance) noexcept
{
    *instance = nullptr;
    auto* storage = host->allocate(
        host->hostContext,
        sizeof(Instance),
        alignof(Instance));
    if (storage == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    *instance = new (storage) Instance{};
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 createOscillator(
    const TalasKernelAbiHostServicesV1* host,
    void** instance) noexcept
{
    return createInstance<OscillatorAbiInstance>(host, instance);
}

TalasKernelAbiStatusV1 createFilter(
    const TalasKernelAbiHostServicesV1* host,
    void** instance) noexcept
{
    const auto result = createInstance<FilterAbiInstance>(host, instance);
    if (result == TALAS_KERNEL_ABI_STATUS_OK)
        static_cast<FilterAbiInstance*>(*instance)->host = host;
    return result;
}

TalasKernelAbiStatusV1 createAdsr(
    const TalasKernelAbiHostServicesV1* host,
    void** instance) noexcept
{
    const auto result = createInstance<AdsrAbiInstance>(host, instance);
    if (result == TALAS_KERNEL_ABI_STATUS_OK)
        static_cast<AdsrAbiInstance*>(*instance)->host = host;
    return result;
}

[[nodiscard]] bool parameterPrefixValid(
    const TalasKernelAbiParameterV1& parameter) noexcept
{
    return parameter.structSize >= sizeof(TalasKernelAbiParameterV1)
        && parameter.reserved0 == 0;
}

TalasKernelAbiStatusV1 prepareOscillator(
    void* opaque,
    const TalasKernelAbiPrepareV1* prepare) noexcept
{
    OscillatorKernelConfig config;
    config.sampleRate = prepare->sampleRate;
    bool sawModulation = false;
    bool sawMaxMi = false;
    bool sawInterpolation = false;
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(prepare->parameterCount);
         ++index)
    {
        const auto& parameter = prepare->parameters[index];
        if (!parameterPrefixValid(parameter))
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        const auto id = text(parameter.id);
        if (id == "modulation" && !sawModulation
            && parameter.kind == TALAS_KERNEL_ABI_PARAMETER_TEXT)
        {
            sawModulation = true;
            const auto value = text(parameter.textValue);
            if (value == "frequency" || value == "fm")
                config.frequencyModulation = true;
            else if (!value.empty() && value != "none" && value != "off")
                return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        }
        else if (id == "maxMI" && !sawMaxMi
            && parameter.kind == TALAS_KERNEL_ABI_PARAMETER_SCALAR)
        {
            sawMaxMi = true;
            config.maxModulationIndex = parameter.scalarValue;
        }
        else if (id == "interpolation" && !sawInterpolation
            && parameter.kind == TALAS_KERNEL_ABI_PARAMETER_TEXT)
        {
            sawInterpolation = true;
            const auto value = text(parameter.textValue);
            if (value == "linear")
            {
                config.interpolation =
                    OscillatorWavetableInterpolation::Linear;
            }
            else if (value == "none" || value == "nearest")
            {
                config.interpolation =
                    OscillatorWavetableInterpolation::Nearest;
            }
            else
            {
                return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
            }
        }
        else
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        }
    }
    return status(
        static_cast<OscillatorAbiInstance*>(opaque)->kernel.prepare(config));
}

TalasKernelAbiStatusV1 prepareFilter(
    void* opaque,
    const TalasKernelAbiPrepareV1* prepare) noexcept
{
    FilterKernelConfig config;
    config.sampleRate = prepare->sampleRate;
    config.stageCount = 4;
    bool sawType = false;
    bool sawOrder = false;
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(prepare->parameterCount);
         ++index)
    {
        const auto& parameter = prepare->parameters[index];
        if (!parameterPrefixValid(parameter))
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        const auto id = text(parameter.id);
        if (id == "type" && !sawType
            && parameter.kind == TALAS_KERNEL_ABI_PARAMETER_TEXT)
        {
            sawType = true;
            const auto value = text(parameter.textValue);
            if (value == "lowPass")
                config.mode = FilterKernelMode::LowPass;
            else if (value == "highPass")
                config.mode = FilterKernelMode::HighPass;
            else if (value == "allPass")
                config.mode = FilterKernelMode::AllPass;
            else
                return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        }
        else if (id == "order" && !sawOrder
            && parameter.kind == TALAS_KERNEL_ABI_PARAMETER_INTEGER
            && parameter.integerValue > 0
            && parameter.integerValue <= 16)
        {
            sawOrder = true;
            config.stageCount =
                static_cast<std::size_t>(parameter.integerValue);
        }
        else
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        }
    }
    auto* instance = static_cast<FilterAbiInstance*>(opaque);
    if (prepare->maximumBlockFrames
        > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max() / sizeof(double)))
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
    }
    const auto capacity = static_cast<std::size_t>(
        prepare->maximumBlockFrames);
    instance->outputScratch = static_cast<double*>(
        instance->host->allocate(
            instance->host->hostContext,
            sizeof(double) * capacity,
            alignof(double)));
    if (instance->outputScratch == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    instance->outputCapacity = capacity;
    return status(instance->kernel.prepare(config));
}

TalasKernelAbiStatusV1 prepareAdsr(
    void* opaque,
    const TalasKernelAbiPrepareV1* prepare) noexcept
{
    auto* instance = static_cast<AdsrAbiInstance*>(opaque);
    bool sawAttack = false;
    bool sawDecay = false;
    bool sawSustain = false;
    bool sawRelease = false;
    bool sawScale = false;
    bool sawLegato = false;
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(prepare->parameterCount);
         ++index)
    {
        const auto& parameter = prepare->parameters[index];
        if (!parameterPrefixValid(parameter)
            || parameter.kind != TALAS_KERNEL_ABI_PARAMETER_SCALAR)
        {
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        }
        const auto id = text(parameter.id);
        double* destination = nullptr;
        if (id == "attackTime" && !sawAttack)
        {
            sawAttack = true;
            destination = &instance->attackTime;
        }
        else if (id == "decayTime" && !sawDecay)
        {
            sawDecay = true;
            destination = &instance->decayTime;
        }
        else if (id == "sustainLevel" && !sawSustain)
        {
            sawSustain = true;
            destination = &instance->sustainLevel;
        }
        else if (id == "releaseTime" && !sawRelease)
        {
            sawRelease = true;
            destination = &instance->releaseTime;
        }
        else if (id == "scale" && !sawScale)
        {
            sawScale = true;
            destination = &instance->scale;
        }
        else if (id == "legato" && !sawLegato)
        {
            sawLegato = true;
            destination = &instance->legato;
        }
        if (destination == nullptr)
            return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
        *destination = parameter.scalarValue;
    }
    if (!std::isfinite(instance->attackTime)
        || instance->attackTime <= 0.0
        || !std::isfinite(instance->decayTime)
        || instance->decayTime <= 0.0
        || !std::isfinite(instance->attackTime + instance->decayTime)
        || !std::isfinite(instance->sustainLevel)
        || instance->sustainLevel < 0.0
        || instance->sustainLevel > 1.0
        || !std::isfinite(instance->releaseTime)
        || instance->releaseTime <= 0.0
        || !std::isfinite(instance->scale)
        || !std::isfinite(instance->legato))
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION;
    }
    constexpr auto capacity = static_cast<std::size_t>(
        TALAS_KERNEL_ABI_MAX_PROCESS_EVENTS_V1);
    instance->eventScratch = static_cast<AdsrGateEvent*>(
        instance->host->allocate(
            instance->host->hostContext,
            sizeof(AdsrGateEvent) * capacity,
            alignof(AdsrGateEvent)));
    if (instance->eventScratch == nullptr)
        return TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR;
    instance->eventCapacity = capacity;
    return status(instance->kernel.prepare({prepare->sampleRate}));
}

TalasKernelAbiStatusV1 resetOscillator(
    void* opaque,
    const TalasKernelAbiResetV1*) noexcept
{
    static_cast<OscillatorAbiInstance*>(opaque)->kernel.reset();
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 resetFilter(
    void* opaque,
    const TalasKernelAbiResetV1*) noexcept
{
    static_cast<FilterAbiInstance*>(opaque)->kernel.reset();
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 resetAdsr(
    void* opaque,
    const TalasKernelAbiResetV1*) noexcept
{
    static_cast<AdsrAbiInstance*>(opaque)->kernel.reset();
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 processOscillator(
    void* opaque,
    const TalasKernelAbiProcessV1* process) noexcept
{
    if (process->inputCount != 6 || process->outputCount != 1
        || process->eventCount != 0 || process->flags != 0)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    }
    OscillatorKernelBlock block;
    block.output = {
        process->outputs[0].samples,
        static_cast<std::size_t>(process->frameCount),
    };
    block.frequency = signal(process->inputs[0]);
    block.amplitude = signal(process->inputs[1]);
    block.envelope = signal(process->inputs[2]);
    block.modulator = signal(process->inputs[3]);
    block.pitchBend = signal(process->inputs[4]);
    block.reset = signal(process->inputs[5]);
    return status(
        static_cast<OscillatorAbiInstance*>(opaque)->kernel.process(block));
}

TalasKernelAbiStatusV1 processFilter(
    void* opaque,
    const TalasKernelAbiProcessV1* process) noexcept
{
    if (process->inputCount != 5 || process->outputCount != 1
        || process->eventCount != 0 || process->flags != 0
        || process->inputs[0].kind != TALAS_KERNEL_ABI_SIGNAL_PER_FRAME)
    {
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    }
    auto* instance = static_cast<FilterAbiInstance*>(opaque);
    if (process->frameCount > instance->outputCapacity)
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    auto candidate = instance->kernel;
    FilterKernelBlock block;
    block.input = {
        process->inputs[0].samples,
        static_cast<std::size_t>(process->frameCount),
    };
    block.output = {
        instance->outputScratch,
        static_cast<std::size_t>(process->frameCount),
    };
    block.frequency = signal(process->inputs[1]);
    block.q = signal(process->inputs[2]);
    block.scale = signal(process->inputs[3]);
    block.feedback = signal(process->inputs[4]);
    const auto result = candidate.process(block);
    if (result != KernelProcessStatus::Ok)
        return status(result);
    std::copy_n(
        instance->outputScratch,
        static_cast<std::size_t>(process->frameCount),
        process->outputs[0].samples);
    instance->kernel = candidate;
    return TALAS_KERNEL_ABI_STATUS_OK;
}

TalasKernelAbiStatusV1 processAdsr(
    void* opaque,
    const TalasKernelAbiProcessV1* process) noexcept
{
    if (process->inputCount != 1 || process->outputCount != 1)
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    auto* instance = static_cast<AdsrAbiInstance*>(opaque);
    if (process->eventCount > instance->eventCapacity)
        return TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT;
    for (std::size_t index = 0;
         index < static_cast<std::size_t>(process->eventCount);
         ++index)
    {
        const auto& source = process->events[index];
        instance->eventScratch[index] = {
            source.targetIndex,
            static_cast<std::size_t>(source.frameOffset),
            source.sequence,
            (source.payloadBits & TALAS_KERNEL_ABI_GATE_EVENT_HIGH) != 0,
        };
    }
    AdsrKernelBlock block;
    block.output = {
        process->outputs[0].samples,
        static_cast<std::size_t>(process->frameCount),
    };
    block.attackTime = KernelSignal::constantValue(instance->attackTime);
    block.decayTime = KernelSignal::constantValue(instance->decayTime);
    block.sustainLevel = KernelSignal::constantValue(instance->sustainLevel);
    block.releaseTime = KernelSignal::constantValue(instance->releaseTime);
    block.scale = KernelSignal::constantValue(instance->scale);
    block.gate = signal(process->inputs[0]);
    block.legato = KernelSignal::constantValue(instance->legato);
    block.typedGateEvents = {
        instance->eventScratch,
        static_cast<std::size_t>(process->eventCount),
    };
    block.typedGateTargetIndex = 1;
    block.useTypedGateEvents =
        (process->flags & TALAS_KERNEL_ABI_PROCESS_TYPED_EVENTS) != 0;
    return status(instance->kernel.process(block));
}

template <typename Instance>
void destroyInstance(
    void* opaque,
    const TalasKernelAbiHostServicesV1* host) noexcept
{
    auto* instance = static_cast<Instance*>(opaque);
    instance->~Instance();
    host->deallocate(
        host->hostContext,
        instance,
        sizeof(Instance),
        alignof(Instance));
}

void destroyOscillator(
    void* opaque,
    const TalasKernelAbiHostServicesV1* host) noexcept
{
    destroyInstance<OscillatorAbiInstance>(opaque, host);
}

void destroyFilter(
    void* opaque,
    const TalasKernelAbiHostServicesV1* host) noexcept
{
    auto* instance = static_cast<FilterAbiInstance*>(opaque);
    if (instance->outputScratch != nullptr)
    {
        host->deallocate(
            host->hostContext,
            instance->outputScratch,
            sizeof(double) * instance->outputCapacity,
            alignof(double));
        instance->outputScratch = nullptr;
    }
    destroyInstance<FilterAbiInstance>(instance, host);
}

void destroyAdsr(
    void* opaque,
    const TalasKernelAbiHostServicesV1* host) noexcept
{
    auto* instance = static_cast<AdsrAbiInstance*>(opaque);
    if (instance->eventScratch != nullptr)
    {
        host->deallocate(
            host->hostContext,
            instance->eventScratch,
            sizeof(AdsrGateEvent) * instance->eventCapacity,
            alignof(AdsrGateEvent));
        instance->eventScratch = nullptr;
    }
    destroyInstance<AdsrAbiInstance>(instance, host);
}

void unload(
    void*,
    const TalasKernelAbiHostServicesV1*) noexcept
{
}

[[nodiscard]] TalasKernelAbiStringViewV1 literal(
    const char* value,
    std::size_t size) noexcept
{
    return {value, size};
}
}

const TalasKernelAbiModuleV1& builtinKernelAbiModule() noexcept
{
    static const std::array descriptors = [] {
        std::array<TalasKernelAbiDescriptorV1, 3> result{};
        result[0] = {
            sizeof(TalasKernelAbiDescriptorV1),
            TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1,
            literal("talas.oscillator", sizeof("talas.oscillator") - 1),
            TALAS_KERNEL_ABI_CAPABILITY_RESET
                | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL,
            createOscillator,
            prepareOscillator,
            resetOscillator,
            processOscillator,
            destroyOscillator,
            0,
            0,
        };
        result[1] = {
            sizeof(TalasKernelAbiDescriptorV1),
            TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1,
            literal("talas.filter", sizeof("talas.filter") - 1),
            TALAS_KERNEL_ABI_CAPABILITY_RESET
                | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL,
            createFilter,
            prepareFilter,
            resetFilter,
            processFilter,
            destroyFilter,
            0,
            0,
        };
        result[2] = {
            sizeof(TalasKernelAbiDescriptorV1),
            TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1,
            literal("talas.adsr", sizeof("talas.adsr") - 1),
            TALAS_KERNEL_ABI_CAPABILITY_RESET
                | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL
                | TALAS_KERNEL_ABI_CAPABILITY_TYPED_EVENTS,
            createAdsr,
            prepareAdsr,
            resetAdsr,
            processAdsr,
            destroyAdsr,
            0,
            0,
        };
        return result;
    }();
    static const TalasKernelAbiModuleV1 module = [] {
        TalasKernelAbiModuleV1 result{};
        result.structSize = sizeof(result);
        result.descriptorVersion = TALAS_KERNEL_ABI_DESCRIPTOR_VERSION_V1;
        result.moduleId = literal(
            "org.talas.builtin-kernels",
            sizeof("org.talas.builtin-kernels") - 1);
        result.moduleVersion = literal("1.0.0", sizeof("1.0.0") - 1);
        result.minimumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        result.maximumApiVersion = TALAS_KERNEL_API_VERSION_V1;
        result.compatibility = currentKernelAbiCompatibility();
        result.sampleFormats = TALAS_KERNEL_ABI_SAMPLE_FORMAT_F64;
        result.trustClass = TALAS_KERNEL_ABI_TRUST_BUILTIN;
        result.capabilityFlags = TALAS_KERNEL_ABI_CAPABILITY_RESET
            | TALAS_KERNEL_ABI_CAPABILITY_STATEFUL
            | TALAS_KERNEL_ABI_CAPABILITY_TYPED_EVENTS;
        result.kernels = descriptors.data();
        result.kernelCount = descriptors.size();
        result.unload = unload;
        return result;
    }();
    return module;
}

const TalasKernelAbiCheckpointExtensionV1& builtinKernelAbiCheckpointExtension() noexcept
{
    static const std::array<TalasKernelAbiCheckpointDescriptorV1, 3> descriptors = {{
        {sizeof(TalasKernelAbiCheckpointDescriptorV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
            literal("talas.oscillator", sizeof("talas.oscillator") - 1),
            captureCheckpoint<OscillatorAbiInstance>, restoreCheckpoint<OscillatorAbiInstance>,
            destroyCheckpoint<OscillatorAbiInstance>, 0, 0},
        {sizeof(TalasKernelAbiCheckpointDescriptorV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
            literal("talas.filter", sizeof("talas.filter") - 1),
            captureCheckpoint<FilterAbiInstance>, restoreCheckpoint<FilterAbiInstance>,
            destroyCheckpoint<FilterAbiInstance>, 0, 0},
        {sizeof(TalasKernelAbiCheckpointDescriptorV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
            literal("talas.adsr", sizeof("talas.adsr") - 1),
            captureCheckpoint<AdsrAbiInstance>, restoreCheckpoint<AdsrAbiInstance>,
            destroyCheckpoint<AdsrAbiInstance>, 0, 0},
    }};
    static const TalasKernelAbiCheckpointExtensionV1 extension = {
        sizeof(TalasKernelAbiCheckpointExtensionV1), TALAS_KERNEL_ABI_CHECKPOINT_VERSION_V1,
        builtinKernelAbiModule().moduleId, builtinKernelAbiModule().moduleVersion,
        descriptors.data(), descriptors.size(), 0, 0,
    };
    return extension;
}
}
