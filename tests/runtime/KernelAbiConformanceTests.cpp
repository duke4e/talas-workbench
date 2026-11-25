#include "talas/runtime/BuiltinKernelAbiModule.h"

#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/KernelAbiHost.h"
#include "talas/runtime/KernelRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
thread_local std::size_t ordinaryAllocations = 0;
}
void* operator new(std::size_t size)
{
    ++ordinaryAllocations;
    if (auto* memory = std::malloc(size == 0 ? 1 : size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace
{
static_assert(TALAS_KERNEL_API_VERSION_V4 == 4);
static_assert(TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V4 == 4);
static_assert(offsetof(TalasKernelAbiHostServicesV4, v3) == 0);
static_assert(sizeof(TalasKernelAbiHostServicesV4) == 128);

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireEqual(
    std::span<const double> actual,
    std::span<const double> expected,
    const std::string& message)
{
    require(actual.size() == expected.size(), message + " size mismatch");
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        if (actual[index] != expected[index])
        {
            throw std::runtime_error(
                message + " at frame " + std::to_string(index));
        }
    }
}

thread_local bool failCheckpointAllocation = false;
thread_local std::size_t hostAllocationCount = 0;

void* allocate(void*, std::uint64_t bytes, std::uint64_t)
{
    ++hostAllocationCount;
    if (failCheckpointAllocation) return nullptr;
    return std::malloc(static_cast<std::size_t>(bytes));
}

void deallocate(
    void*,
    void* allocation,
    std::uint64_t,
    std::uint64_t)
{
    std::free(allocation);
}

void diagnostic(
    void*,
    TalasKernelAbiDiagnosticSeverityV1,
    std::uint32_t,
    TalasKernelAbiStringViewV1)
{
}

TalasKernelAbiHostServicesV1 hostServices()
{
    TalasKernelAbiHostServicesV1 services{};
    services.structSize = sizeof(services);
    services.servicesVersion = TALAS_KERNEL_ABI_HOST_SERVICES_VERSION_V1;
    services.allocate = allocate;
    services.deallocate = deallocate;
    services.diagnostic = diagnostic;
    return services;
}

TalasKernelAbiPrepareV1 prepareRecord(double sampleRate, std::size_t maximumFrames)
{
    TalasKernelAbiPrepareV1 prepare{};
    prepare.structSize = sizeof(prepare);
    prepare.apiVersion = TALAS_KERNEL_API_VERSION_V1;
    prepare.sampleRate = sampleRate;
    prepare.maximumBlockFrames = maximumFrames;
    return prepare;
}

TalasKernelAbiStringViewV1 text(std::string_view value)
{
    return {value.data(), value.size()};
}

TalasKernelAbiParameterV1 scalarParameter(
    std::string_view id,
    double value)
{
    TalasKernelAbiParameterV1 parameter{};
    parameter.structSize = sizeof(parameter);
    parameter.kind = TALAS_KERNEL_ABI_PARAMETER_SCALAR;
    parameter.id = text(id);
    parameter.scalarValue = value;
    return parameter;
}

TalasKernelAbiParameterV1 integerParameter(
    std::string_view id,
    std::int64_t value)
{
    TalasKernelAbiParameterV1 parameter{};
    parameter.structSize = sizeof(parameter);
    parameter.kind = TALAS_KERNEL_ABI_PARAMETER_INTEGER;
    parameter.id = text(id);
    parameter.integerValue = value;
    return parameter;
}

TalasKernelAbiParameterV1 textParameter(
    std::string_view id,
    std::string_view value)
{
    TalasKernelAbiParameterV1 parameter{};
    parameter.structSize = sizeof(parameter);
    parameter.kind = TALAS_KERNEL_ABI_PARAMETER_TEXT;
    parameter.id = text(id);
    parameter.textValue = text(value);
    return parameter;
}

TalasKernelAbiSignalV1 constant(double value)
{
    TalasKernelAbiSignalV1 signal{};
    signal.structSize = sizeof(signal);
    signal.kind = TALAS_KERNEL_ABI_SIGNAL_CONSTANT;
    signal.constantValue = value;
    return signal;
}

TalasKernelAbiSignalV1 samples(std::span<const double> values)
{
    TalasKernelAbiSignalV1 signal{};
    signal.structSize = sizeof(signal);
    signal.kind = TALAS_KERNEL_ABI_SIGNAL_PER_FRAME;
    signal.samples = values.data();
    signal.sampleCount = values.size();
    return signal;
}

TalasKernelAbiTransportV1 transport(double sampleRate)
{
    TalasKernelAbiTransportV1 record{};
    record.structSize = sizeof(record);
    record.sampleRate = sampleRate;
    return record;
}

TalasKernelAbiProcessV1 processRecord(
    double sampleRate,
    std::size_t frames,
    std::span<const TalasKernelAbiSignalV1> inputs,
    TalasKernelAbiOutputV1& output,
    std::span<const TalasKernelAbiEventV1> events = {},
    TalasKernelAbiProcessFlagsV1 flags = 0)
{
    TalasKernelAbiProcessV1 block{};
    block.structSize = sizeof(block);
    block.flags = flags;
    block.frameCount = frames;
    block.inputs = inputs.data();
    block.inputCount = inputs.size();
    block.outputs = &output;
    block.outputCount = 1;
    block.events = events.data();
    block.eventCount = events.size();
    block.transport = transport(sampleRate);
    return block;
}

TalasKernelAbiResetV1 resetRecord(double sampleRate)
{
    TalasKernelAbiResetV1 reset{};
    reset.structSize = sizeof(reset);
    reset.reason = TALAS_KERNEL_ABI_RESET_EXPLICIT;
    reset.transport = transport(sampleRate);
    return reset;
}

void testBuiltinOfflineCheckpointContinuation()
{
    constexpr double rate = 1000.0;
    constexpr std::size_t frames = 17;
    for (const auto type : {"talas.oscillator", "talas.filter", "talas.adsr"})
    for (const bool typed : {false, true})
    {
        if (typed && std::string_view(type) != "talas.adsr") continue;
        for (const auto prefix : {std::size_t{0}, std::size_t{7}})
        {
            talas::runtime::KernelAbiHost host(talas::core::makeBuiltinNodeRegistry(), hostServices());
            require(host.registerModule(talas::runtime::builtinKernelAbiModule(),
                &talas::runtime::builtinKernelAbiCheckpointExtension()).ok(),
                "checkpoint module should register with its explicit extension");
            auto prepared = prepareRecord(rate, frames);
            const std::array adsrParameters = {
                scalarParameter("attackTime", .01), scalarParameter("decayTime", .02),
                scalarParameter("sustainLevel", .3), scalarParameter("releaseTime", .012),
            };
            if (std::string_view(type) == "talas.adsr")
            {
                prepared.parameters = adsrParameters.data();
                prepared.parameterCount = adsrParameters.size();
            }
            const auto candidate = host.createPreparedInstance(
                "org.talas.builtin-kernels", type, prepared);
            require(candidate.ok() && candidate.instance->supportsOfflineCheckpoint(),
                "each builtin family should expose the validated checkpoint capability");
            talas::runtime::OscillatorKernel oscillator;
            talas::runtime::FilterKernel filter;
            talas::runtime::AdsrKernel adsr;
            require(oscillator.prepare({.sampleRate = rate}) == talas::runtime::KernelProcessStatus::Ok
                && filter.prepare({rate, talas::runtime::FilterKernelMode::LowPass, 4}) == talas::runtime::KernelProcessStatus::Ok
                && adsr.prepare({rate}) == talas::runtime::KernelProcessStatus::Ok,
                "independent direct kernels should prepare");
            std::array<double, 64> source{}, gates{};
            for (std::size_t frame = 0; frame < source.size(); ++frame)
            {
                source[frame] = std::sin(double(frame) * .31) * .6 + (frame == 0 ? .4 : 0);
                gates[frame] = frame < 12 || frame >= 23 ? 1.0 : 0.0;
            }
            const auto run = [&](std::size_t start, std::size_t count, bool direct) {
                std::vector<double> output(count);
                const auto input = std::span(source).subspan(start, count);
                const auto gate = std::span(gates).subspan(start, count);
                std::vector<talas::runtime::AdsrGateEvent> directEvents;
                std::vector<TalasKernelAbiEventV1> events;
                if (typed)
                    for (const auto edge : {std::size_t{0}, std::size_t{12}, std::size_t{23}})
                        if (edge >= start && edge < start + count)
                        {
                            directEvents.push_back({1, edge - start, edge + 1, edge != 12});
                            TalasKernelAbiEventV1 event{};
                            event.structSize = sizeof(event); event.kind = TALAS_KERNEL_ABI_EVENT_GATE;
                            event.targetIndex = 1; event.frameOffset = edge - start; event.sequence = edge + 1;
                            event.payloadBits = edge != 12 ? TALAS_KERNEL_ABI_GATE_EVENT_HIGH : 0;
                            events.push_back(event);
                        }
                if (direct)
                {
                    talas::runtime::KernelProcessStatus result{};
                    if (std::string_view(type) == "talas.oscillator")
                    {
                        talas::runtime::OscillatorKernelBlock block;
                        block.output = output;
                        block.frequency = talas::runtime::KernelSignal::constantValue(13);
                        block.amplitude = talas::runtime::KernelSignal::constantValue(.7);
                        block.envelope = talas::runtime::KernelSignal::constantValue(.8);
                        result = oscillator.process(block);
                    }
                    else if (std::string_view(type) == "talas.filter")
                    {
                        talas::runtime::FilterKernelBlock block;
                        block.input = input; block.output = output;
                        block.frequency = talas::runtime::KernelSignal::constantValue(90);
                        block.q = talas::runtime::KernelSignal::constantValue(.707);
                        block.scale = talas::runtime::KernelSignal::constantValue(.8);
                        block.feedback = talas::runtime::KernelSignal::constantValue(.2);
                        result = filter.process(block);
                    }
                    else
                    {
                        talas::runtime::AdsrKernelBlock block;
                        block.output = output; block.gate = talas::runtime::KernelSignal::sampleValues(gate);
                        block.attackTime = talas::runtime::KernelSignal::constantValue(.01);
                        block.decayTime = talas::runtime::KernelSignal::constantValue(.02);
                        block.sustainLevel = talas::runtime::KernelSignal::constantValue(.3);
                        block.releaseTime = talas::runtime::KernelSignal::constantValue(.012);
                        block.useTypedGateEvents = typed;
                        block.typedGateTargetIndex = 1;
                        block.typedGateEvents = directEvents;
                        result = adsr.process(block);
                    }
                    require(result == talas::runtime::KernelProcessStatus::Ok, "direct checkpoint oracle should render");
                }
                else
                {
                    std::vector<TalasKernelAbiSignalV1> inputs;
                    if (std::string_view(type) == "talas.oscillator")
                        inputs = {constant(13), constant(.7), constant(.8), constant(0), constant(0), constant(0)};
                    else if (std::string_view(type) == "talas.filter")
                        inputs = {samples(input), constant(90), constant(.707), constant(.8), constant(.2)};
                    else inputs = {samples(gate)};
                    TalasKernelAbiOutputV1 record{};
                    record.structSize = sizeof(record); record.samples = output.data(); record.sampleCount = count;
                    require(candidate.instance->process(processRecord(rate, count, inputs, record, events,
                        typed ? TALAS_KERNEL_ABI_PROCESS_TYPED_EVENTS : 0)) == TALAS_KERNEL_ABI_STATUS_OK,
                        "checkpoint candidate should render");
                }
                return output;
            };
            if (prefix != 0) requireEqual(run(0, prefix, false), run(0, prefix, true), "checkpoint prefix");
            failCheckpointAllocation = true;
            const auto failed = candidate.instance->beginOfflineCheckpoint();
            failCheckpointAllocation = false;
            require(failed == TALAS_KERNEL_ABI_STATUS_INTERNAL_ERROR && !candidate.instance->isQuarantined(),
                "failed checkpoint allocation must preserve a retryable kernel");
            const auto expected = run(prefix, frames, true);
            if (std::string_view(type) == "talas.oscillator")
                for (std::size_t frame = 0; frame < frames; ++frame)
                    require(std::abs(expected[frame] - std::sin(2 * std::acos(-1.0) * 13 * double(prefix + frame) / rate) * .56) < 1e-12,
                        "checkpoint oscillator oracle must agree with explicit phase");
            if (std::string_view(type) == "talas.adsr" && prefix == 0)
                for (std::size_t frame = 0; frame <= 10; ++frame)
                    require(std::abs(expected[frame] - double(frame) / 10) < 1e-12,
                        "checkpoint envelope must follow the explicit attack ramp");
            for (int attempt = 0; attempt < 2; ++attempt)
            {
                require(candidate.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK,
                    "kernel must capture its processing boundary");
                require(candidate.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                    "nested capture must retain the original baseline");
                requireEqual(run(prefix, frames, false), expected, "provisional continuation before restore");
                require(candidate.instance->reset(resetRecord(rate)) == TALAS_KERNEL_ABI_STATUS_OK,
                    "provisional reset should change only provisional DSP state");
                static_cast<void>(run(31, frames, false));
                const auto allocations = hostAllocationCount;
                const auto ordinaryBefore = ordinaryAllocations;
                const auto restored = candidate.instance->restoreOfflineCheckpoint();
                const bool noAllocation = ordinaryAllocations == ordinaryBefore;
                require(restored == TALAS_KERNEL_ABI_STATUS_OK && noAllocation,
                    "kernel processing history should restore without ordinary allocations");
                require(hostAllocationCount == allocations, "restore and snapshot destruction must not allocate");
            }
            require(candidate.instance->beginOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_OK, "retry should capture");
            requireEqual(run(prefix, frames, false), expected, "restored continuation after repeated aborts");
            const auto allocations = hostAllocationCount;
            const auto ordinaryBefore = ordinaryAllocations;
            const auto committed = candidate.instance->commitOfflineCheckpoint();
            const bool noAllocation = ordinaryAllocations == ordinaryBefore;
            require(committed == TALAS_KERNEL_ABI_STATUS_OK && noAllocation,
                "commit should retain processing history without ordinary allocations");
            require(hostAllocationCount == allocations, "commit must not allocate");
            require(candidate.instance->restoreOfflineCheckpoint() == TALAS_KERNEL_ABI_STATUS_INVALID_ARGUMENT,
                "consumed snapshot must not restore twice");
            requireEqual(run(prefix + frames, frames, false), run(prefix + frames, frames, true),
                "committed continuation must preserve history after snapshot destruction");
        }
    }
}

void testStaticModuleMetadataAndOscillatorParity()
{
    auto services = hostServices();
    const auto registry = talas::core::makeBuiltinNodeRegistry();
    talas::runtime::KernelAbiHost host(
        registry,
        services);
    const auto registered = host.registerModule(
        talas::runtime::builtinKernelAbiModule());
    require(registered.ok(), "the built-in ABI module should register");
    require(
        registered.module->kernels().size() == 3,
        "the built-in ABI module should expose exactly the three approved families");
    for (const auto& kernel : registered.module->kernels())
    {
        const auto* node = registry.findNodeType(kernel.typeId);
        require(
            node != nullptr
                && talas::runtime::findBuiltinKernel(kernel.typeId) != nullptr,
            "every ABI descriptor should resolve through both authorities");
        TalasKernelAbiCapabilityFlagsV1 expected = 0;
        if (node->capabilities.supportsReset)
            expected |= TALAS_KERNEL_ABI_CAPABILITY_RESET;
        if (node->capabilities.state
            == talas::core::NodeStateModel::SessionStateful)
        {
            expected |= TALAS_KERNEL_ABI_CAPABILITY_STATEFUL;
        }
        if (node->capabilities.transportAware)
            expected |= TALAS_KERNEL_ABI_CAPABILITY_TRANSPORT;
        if (node->capabilities.typedEvents)
            expected |= TALAS_KERNEL_ABI_CAPABILITY_TYPED_EVENTS;
        require(
            kernel.capabilityFlags == expected,
            "ABI capabilities must match NodeRegistry exactly");
    }

    constexpr double sampleRate = 48'000.0;
    constexpr std::size_t frames = 7;
    auto abi = host.createPreparedInstance(
        "org.talas.builtin-kernels",
        "talas.oscillator",
        prepareRecord(sampleRate, frames));
    require(abi.ok(), "the ABI oscillator should prepare");

    talas::runtime::OscillatorKernel direct;
    require(
        direct.prepare({.sampleRate = sampleRate})
            == talas::runtime::KernelProcessStatus::Ok,
        "the direct oscillator should prepare");
    std::array<double, frames> reset{};
    reset[3] = 1.0;
    std::array<double, frames> inactiveModulator{};
    inactiveModulator.fill(std::numeric_limits<double>::quiet_NaN());
    std::array<double, frames> directOutput{};
    std::array<double, frames> abiOutput{};
    talas::runtime::OscillatorKernelBlock directBlock;
    directBlock.output = directOutput;
    directBlock.frequency = talas::runtime::KernelSignal::constantValue(440.0);
    directBlock.amplitude = talas::runtime::KernelSignal::constantValue(0.5);
    directBlock.envelope = talas::runtime::KernelSignal::constantValue(0.8);
    directBlock.modulator =
        talas::runtime::KernelSignal::sampleValues(inactiveModulator);
    directBlock.pitchBend = talas::runtime::KernelSignal::constantValue(0.0);
    directBlock.reset = talas::runtime::KernelSignal::sampleValues(reset);
    require(
        direct.process(directBlock) == talas::runtime::KernelProcessStatus::Ok,
        "the direct oscillator should process");

    const std::array abiInputs = {
        constant(440.0),
        constant(0.5),
        constant(0.8),
        samples(inactiveModulator),
        constant(0.0),
        samples(reset),
    };
    TalasKernelAbiOutputV1 output{};
    output.structSize = sizeof(output);
    output.samples = abiOutput.data();
    output.sampleCount = abiOutput.size();
    const auto block = processRecord(
        sampleRate,
        frames,
        abiInputs,
        output);
    require(
        abi.instance->process(block) == TALAS_KERNEL_ABI_STATUS_OK,
        "the ABI oscillator should process");
    requireEqual(abiOutput, directOutput,
        "the ABI oscillator must delegate exactly to the shared kernel");

    std::array<double, frames> directRejected{};
    std::array<double, frames> abiRejected{};
    directRejected.fill(9.0);
    abiRejected.fill(9.0);
    std::array<double, frames - 1> truncatedFrequency{};
    talas::runtime::OscillatorKernelBlock rejectedDirect;
    rejectedDirect.output = directRejected;
    rejectedDirect.frequency =
        talas::runtime::KernelSignal::sampleValues(truncatedFrequency);
    require(
        direct.process(rejectedDirect)
            == talas::runtime::KernelProcessStatus::InvalidBlockLayout,
        "direct oscillator should reject a truncated signal");
    auto rejectedInputs = abiInputs;
    rejectedInputs[0] = samples(truncatedFrequency);
    TalasKernelAbiOutputV1 rejectedOutput{};
    rejectedOutput.structSize = sizeof(rejectedOutput);
    rejectedOutput.samples = abiRejected.data();
    rejectedOutput.sampleCount = abiRejected.size();
    require(
        abi.instance->process(processRecord(
            sampleRate,
            frames,
            rejectedInputs,
            rejectedOutput)) == TALAS_KERNEL_ABI_STATUS_INVALID_BLOCK_LAYOUT,
        "ABI oscillator should reject a truncated signal");
    require(
        directRejected == std::array<double, frames>{9.0, 9.0, 9.0, 9.0,
            9.0, 9.0, 9.0}
            && abiRejected == directRejected,
        "oscillator layout rejection should not mutate output");

    std::array<double, 1> directRecovery{};
    std::array<double, 1> abiRecovery{};
    talas::runtime::OscillatorKernelBlock directRecoveryBlock;
    directRecoveryBlock.output = directRecovery;
    directRecoveryBlock.frequency =
        talas::runtime::KernelSignal::constantValue(440.0);
    directRecoveryBlock.amplitude =
        talas::runtime::KernelSignal::constantValue(0.5);
    directRecoveryBlock.envelope =
        talas::runtime::KernelSignal::constantValue(0.8);
    directRecoveryBlock.modulator =
        talas::runtime::KernelSignal::constantValue(0.0);
    directRecoveryBlock.pitchBend =
        talas::runtime::KernelSignal::constantValue(0.0);
    directRecoveryBlock.reset =
        talas::runtime::KernelSignal::constantValue(0.0);
    require(
        direct.process(directRecoveryBlock)
            == talas::runtime::KernelProcessStatus::Ok,
        "direct oscillator should recover after rejection");
    const std::array recoveryInputs = {
        constant(440.0),
        constant(0.5),
        constant(0.8),
        constant(0.0),
        constant(0.0),
        constant(0.0),
    };
    TalasKernelAbiOutputV1 recoveryOutput{};
    recoveryOutput.structSize = sizeof(recoveryOutput);
    recoveryOutput.samples = abiRecovery.data();
    recoveryOutput.sampleCount = 1;
    require(
        abi.instance->process(processRecord(
            sampleRate,
            1,
            recoveryInputs,
            recoveryOutput)) == TALAS_KERNEL_ABI_STATUS_OK,
        "ABI oscillator should recover after rejection");
    requireEqual(
        abiRecovery,
        directRecovery,
        "oscillator rejection must not advance kernel state");

    std::array<double, 2> directOverflow{95.0, 95.0};
    std::array<double, 2> abiOverflow{95.0, 95.0};
    talas::runtime::OscillatorKernelBlock directOverflowBlock;
    directOverflowBlock.output = directOverflow;
    directOverflowBlock.frequency =
        talas::runtime::KernelSignal::constantValue(440.0);
    directOverflowBlock.amplitude =
        talas::runtime::KernelSignal::constantValue(
            std::numeric_limits<double>::max());
    directOverflowBlock.envelope =
        talas::runtime::KernelSignal::constantValue(2.0);
    require(
        direct.process(directOverflowBlock)
            == talas::runtime::KernelProcessStatus::InvalidValue,
        "C95 direct oscillator should reject derived output overflow");
    const std::array overflowInputs = {
        constant(440.0),
        constant(std::numeric_limits<double>::max()),
        constant(2.0),
        constant(0.0),
        constant(0.0),
        constant(0.0),
    };
    TalasKernelAbiOutputV1 overflowOutput{};
    overflowOutput.structSize = sizeof(overflowOutput);
    overflowOutput.samples = abiOverflow.data();
    overflowOutput.sampleCount = abiOverflow.size();
    require(
        abi.instance->process(processRecord(
            sampleRate,
            abiOverflow.size(),
            overflowInputs,
            overflowOutput)) == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE,
        "C95 ABI oscillator should reject the same derived output overflow");
    require(
        directOverflow == std::array<double, 2>{95.0, 95.0}
            && abiOverflow == directOverflow,
        "C95 direct and ABI overflow rejection should preserve output exactly");

    std::array<double, 2> directOverflowRetry{};
    std::array<double, 2> abiOverflowRetry{};
    directRecoveryBlock.output = directOverflowRetry;
    require(
        direct.process(directRecoveryBlock)
            == talas::runtime::KernelProcessStatus::Ok,
        "C95 direct oscillator should recover after overflow rejection");
    recoveryOutput.samples = abiOverflowRetry.data();
    recoveryOutput.sampleCount = abiOverflowRetry.size();
    require(
        abi.instance->process(processRecord(
            sampleRate,
            abiOverflowRetry.size(),
            recoveryInputs,
            recoveryOutput)) == TALAS_KERNEL_ABI_STATUS_OK,
        "C95 ABI oscillator should recover after overflow rejection");
    requireEqual(
        abiOverflowRetry,
        directOverflowRetry,
        "C95 direct and ABI oscillator retries should preserve exact state parity");
}

void testOscillatorPartitionResetAndModulationParity()
{
    constexpr std::array sampleRates = {44'100.0, 48'000.0, 96'000.0};
    constexpr std::array<std::size_t, 4> partitions = {1, 7, 64, 257};
    constexpr std::size_t totalFrames = 329;
    for (const auto sampleRate : sampleRates)
    {
        auto services = hostServices();
        talas::runtime::KernelAbiHost host(
            talas::core::makeBuiltinNodeRegistry(),
            services);
        require(host.registerModule(talas::runtime::builtinKernelAbiModule()).ok(),
            "oscillator partition module should register");
        const std::array parameters = {
            textParameter("modulation", "frequency"),
            scalarParameter("maxMI", 0.2),
            textParameter("interpolation", "linear"),
        };
        auto prepare = prepareRecord(sampleRate, partitions.back());
        prepare.parameters = parameters.data();
        prepare.parameterCount = parameters.size();
        auto abi = host.createPreparedInstance(
            "org.talas.builtin-kernels",
            "talas.oscillator",
            prepare);
        require(abi.ok(), "partitioned ABI oscillator should prepare");

        talas::runtime::OscillatorKernel direct;
        require(
            direct.prepare({
                .sampleRate = sampleRate,
                .frequencyModulation = true,
                .maxModulationIndex = 0.2,
            }) == talas::runtime::KernelProcessStatus::Ok,
            "whole direct oscillator should prepare");
        std::vector<double> frequency(totalFrames);
        std::vector<double> modulator(totalFrames);
        std::vector<double> reset(totalFrames, 0.0);
        for (std::size_t frame = 0; frame < totalFrames; ++frame)
        {
            frequency[frame] = 110.0 + static_cast<double>(frame % 23U);
            modulator[frame] = 0.25 * std::sin(static_cast<double>(frame));
        }
        reset[8] = 1.0;
        reset[72] = 1.0;
        std::vector<double> directOutput(totalFrames);
        std::vector<double> abiOutput(totalFrames);
        talas::runtime::OscillatorKernelBlock directBlock;
        directBlock.output = directOutput;
        directBlock.frequency =
            talas::runtime::KernelSignal::sampleValues(frequency);
        directBlock.amplitude =
            talas::runtime::KernelSignal::constantValue(0.4);
        directBlock.envelope =
            talas::runtime::KernelSignal::constantValue(0.75);
        directBlock.modulator =
            talas::runtime::KernelSignal::sampleValues(modulator);
        directBlock.pitchBend =
            talas::runtime::KernelSignal::constantValue(0.0);
        directBlock.reset = talas::runtime::KernelSignal::sampleValues(reset);
        require(
            direct.process(directBlock) == talas::runtime::KernelProcessStatus::Ok,
            "whole direct oscillator should process");

        std::size_t offset = 0;
        for (const auto frames : partitions)
        {
            const auto frequencyPart = std::span<const double>(frequency)
                .subspan(offset, frames);
            const auto modulatorPart = std::span<const double>(modulator)
                .subspan(offset, frames);
            const auto resetPart = std::span<const double>(reset)
                .subspan(offset, frames);
            const std::array inputs = {
                samples(frequencyPart),
                constant(0.4),
                constant(0.75),
                samples(modulatorPart),
                constant(0.0),
                samples(resetPart),
            };
            TalasKernelAbiOutputV1 output{};
            output.structSize = sizeof(output);
            output.samples = abiOutput.data() + offset;
            output.sampleCount = frames;
            const auto block = processRecord(
                sampleRate,
                frames,
                inputs,
                output);
            require(
                abi.instance->process(block) == TALAS_KERNEL_ABI_STATUS_OK,
                "partitioned ABI oscillator should process");
            offset += frames;
        }
        requireEqual(
            abiOutput,
            directOutput,
            "oscillator must be partition invariant through the ABI");

        direct.reset();
        require(
            abi.instance->reset(resetRecord(sampleRate))
                == TALAS_KERNEL_ABI_STATUS_OK,
            "ABI oscillator reset should succeed");
        std::array<double, 1> directResetOutput{};
        std::array<double, 1> abiResetOutput{};
        talas::runtime::OscillatorKernelBlock directResetBlock;
        directResetBlock.output = directResetOutput;
        directResetBlock.frequency =
            talas::runtime::KernelSignal::constantValue(220.0);
        directResetBlock.amplitude =
            talas::runtime::KernelSignal::constantValue(1.0);
        directResetBlock.envelope =
            talas::runtime::KernelSignal::constantValue(1.0);
        directResetBlock.modulator =
            talas::runtime::KernelSignal::constantValue(0.0);
        directResetBlock.pitchBend =
            talas::runtime::KernelSignal::constantValue(0.0);
        directResetBlock.reset =
            talas::runtime::KernelSignal::constantValue(0.0);
        require(
            direct.process(directResetBlock)
                == talas::runtime::KernelProcessStatus::Ok,
            "reset direct oscillator should process");
        const std::array resetInputs = {
            constant(220.0),
            constant(1.0),
            constant(1.0),
            constant(0.0),
            constant(0.0),
            constant(0.0),
        };
        TalasKernelAbiOutputV1 resetOutput{};
        resetOutput.structSize = sizeof(resetOutput);
        resetOutput.samples = abiResetOutput.data();
        resetOutput.sampleCount = 1;
        require(
            abi.instance->process(processRecord(
                sampleRate,
                1,
                resetInputs,
                resetOutput)) == TALAS_KERNEL_ABI_STATUS_OK,
            "reset ABI oscillator should process");
        requireEqual(
            abiResetOutput,
            directResetOutput,
            "ABI oscillator reset should restore fresh phase");
    }
}

void testFilterModesOrdersStateAndRejectionParity()
{
    constexpr std::array sampleRates = {44'100.0, 48'000.0, 96'000.0};
    constexpr std::array<std::size_t, 4> frameCounts = {1, 7, 64, 257};
    struct Mode
    {
        std::string_view name;
        talas::runtime::FilterKernelMode value;
        std::int64_t order;
    };
    constexpr std::array modes = {
        Mode{"lowPass", talas::runtime::FilterKernelMode::LowPass, 1},
        Mode{"highPass", talas::runtime::FilterKernelMode::HighPass, 4},
        Mode{"allPass", talas::runtime::FilterKernelMode::AllPass, 16},
    };
    for (const auto sampleRate : sampleRates)
    {
        for (const auto frames : frameCounts)
        {
            for (const auto& mode : modes)
            {
                auto services = hostServices();
                talas::runtime::KernelAbiHost host(
                    talas::core::makeBuiltinNodeRegistry(),
                    services);
                require(host.registerModule(
                            talas::runtime::builtinKernelAbiModule()).ok(),
                    "filter module should register");
                const std::array parameters = {
                    textParameter("type", mode.name),
                    integerParameter("order", mode.order),
                };
                auto prepare = prepareRecord(sampleRate, frames);
                prepare.parameters = parameters.data();
                prepare.parameterCount = parameters.size();
                auto abi = host.createPreparedInstance(
                    "org.talas.builtin-kernels",
                    "talas.filter",
                    prepare);
                require(abi.ok(), "ABI filter should prepare mode/order");

                talas::runtime::FilterKernel direct;
                require(
                    direct.prepare({
                        .sampleRate = sampleRate,
                        .mode = mode.value,
                        .stageCount = static_cast<std::size_t>(mode.order),
                    }) == talas::runtime::KernelProcessStatus::Ok,
                    "direct filter should prepare mode/order");
                std::vector<double> input(frames);
                for (std::size_t frame = 0; frame < frames; ++frame)
                {
                    input[frame] = frame == 0
                        ? 1.0
                        : 0.2 * std::sin(static_cast<double>(frame));
                }
                for (std::size_t pass = 0; pass < 2; ++pass)
                {
                    std::vector<double> directOutput(frames);
                    std::vector<double> abiOutput(frames);
                    talas::runtime::FilterKernelBlock directBlock;
                    directBlock.input = input;
                    directBlock.output = directOutput;
                    directBlock.frequency =
                        talas::runtime::KernelSignal::constantValue(
                            sampleRate * 0.08);
                    directBlock.q =
                        talas::runtime::KernelSignal::constantValue(0.9);
                    directBlock.scale =
                        talas::runtime::KernelSignal::constantValue(0.75);
                    directBlock.feedback =
                        talas::runtime::KernelSignal::constantValue(0.2);
                    require(
                        direct.process(directBlock)
                            == talas::runtime::KernelProcessStatus::Ok,
                        "direct filter should process stateful pass");
                    const std::array inputs = {
                        samples(input),
                        constant(sampleRate * 0.08),
                        constant(0.9),
                        constant(0.75),
                        constant(0.2),
                    };
                    TalasKernelAbiOutputV1 output{};
                    output.structSize = sizeof(output);
                    output.samples = abiOutput.data();
                    output.sampleCount = frames;
                    require(
                        abi.instance->process(processRecord(
                            sampleRate,
                            frames,
                            inputs,
                            output)) == TALAS_KERNEL_ABI_STATUS_OK,
                        "ABI filter should process stateful pass");
                    requireEqual(
                        abiOutput,
                        directOutput,
                        "ABI filter should preserve shared-kernel state exactly");
                }

                if (frames == 7 && mode.value
                        == talas::runtime::FilterKernelMode::LowPass)
                {
                    std::vector<double> directRejected(frames, 9.0);
                    std::vector<double> abiRejected(frames, 9.0);
                    talas::runtime::FilterKernelBlock rejected;
                    rejected.input = input;
                    rejected.output = directRejected;
                    rejected.frequency =
                        talas::runtime::KernelSignal::constantValue(
                            std::numeric_limits<double>::quiet_NaN());
                    rejected.q =
                        talas::runtime::KernelSignal::constantValue(0.9);
                    rejected.scale =
                        talas::runtime::KernelSignal::constantValue(0.75);
                    rejected.feedback =
                        talas::runtime::KernelSignal::constantValue(0.2);
                    require(
                        direct.process(rejected)
                            == talas::runtime::KernelProcessStatus::InvalidValue,
                        "direct filter should reject invalid frequency");
                    const std::array rejectedInputs = {
                        samples(input),
                        constant(std::numeric_limits<double>::quiet_NaN()),
                        constant(0.9),
                        constant(0.75),
                        constant(0.2),
                    };
                    TalasKernelAbiOutputV1 rejectedOutput{};
                    rejectedOutput.structSize = sizeof(rejectedOutput);
                    rejectedOutput.samples = abiRejected.data();
                    rejectedOutput.sampleCount = frames;
                    require(
                        abi.instance->process(processRecord(
                            sampleRate,
                            frames,
                            rejectedInputs,
                            rejectedOutput)) == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE,
                        "ABI filter should map invalid-value rejection");
                    require(
                        directRejected == std::vector<double>(frames, 9.0)
                            && abiRejected
                                == std::vector<double>(frames, 9.0),
                        "filter rejection should not mutate either output");
                }
            }
        }
    }
}

void testFilterPartitionAndFeedbackParity()
{
    constexpr std::array sampleRates = {44'100.0, 48'000.0, 96'000.0};
    constexpr std::array<std::size_t, 4> partitions = {1, 7, 64, 257};
    constexpr std::size_t totalFrames = 329;
    for (const auto sampleRate : sampleRates)
    {
        auto services = hostServices();
        talas::runtime::KernelAbiHost host(
            talas::core::makeBuiltinNodeRegistry(),
            services);
        require(host.registerModule(talas::runtime::builtinKernelAbiModule()).ok(),
            "partitioned filter module should register");
        const std::array parameters = {
            textParameter("type", "lowPass"),
            integerParameter("order", 4),
        };
        auto prepare = prepareRecord(sampleRate, partitions.back());
        prepare.parameters = parameters.data();
        prepare.parameterCount = parameters.size();
        auto abi = host.createPreparedInstance(
            "org.talas.builtin-kernels",
            "talas.filter",
            prepare);
        require(abi.ok(), "partitioned ABI filter should prepare");
        talas::runtime::FilterKernel direct;
        require(
            direct.prepare({
                .sampleRate = sampleRate,
                .mode = talas::runtime::FilterKernelMode::LowPass,
                .stageCount = 4,
            }) == talas::runtime::KernelProcessStatus::Ok,
            "whole direct filter should prepare");

        std::vector<double> input(totalFrames);
        for (std::size_t frame = 0; frame < totalFrames; ++frame)
        {
            input[frame] = frame == 0
                ? 1.0
                : std::sin(static_cast<double>(frame) * 0.13) * 0.1;
        }
        std::vector<double> directOutput(totalFrames);
        std::vector<double> abiOutput(totalFrames);
        talas::runtime::FilterKernelBlock directBlock;
        directBlock.input = input;
        directBlock.output = directOutput;
        directBlock.frequency =
            talas::runtime::KernelSignal::constantValue(sampleRate * 0.06);
        directBlock.q = talas::runtime::KernelSignal::constantValue(0.8);
        directBlock.scale = talas::runtime::KernelSignal::constantValue(0.9);
        directBlock.feedback = talas::runtime::KernelSignal::constantValue(0.35);
        require(
            direct.process(directBlock) == talas::runtime::KernelProcessStatus::Ok,
            "whole direct feedback filter should process");

        std::size_t offset = 0;
        for (const auto frames : partitions)
        {
            const auto inputPart = std::span<const double>(input)
                .subspan(offset, frames);
            const std::array inputs = {
                samples(inputPart),
                constant(sampleRate * 0.06),
                constant(0.8),
                constant(0.9),
                constant(0.35),
            };
            TalasKernelAbiOutputV1 output{};
            output.structSize = sizeof(output);
            output.samples = abiOutput.data() + offset;
            output.sampleCount = frames;
            require(
                abi.instance->process(processRecord(
                    sampleRate,
                    frames,
                    inputs,
                    output)) == TALAS_KERNEL_ABI_STATUS_OK,
                "partitioned ABI feedback filter should process");
            offset += frames;
        }
        requireEqual(
            abiOutput,
            directOutput,
            "filter feedback state should be partition invariant through the ABI");
    }
}

void testFilterLateFailureIsOutputAndStateAtomic()
{
    constexpr double sampleRate = 48'000.0;
    auto services = hostServices();
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        services);
    require(host.registerModule(talas::runtime::builtinKernelAbiModule()).ok(),
        "atomic filter module should register");
    const std::array parameters = {
        textParameter("type", "highPass"),
        integerParameter("order", 1),
    };
    auto prepare = prepareRecord(sampleRate, 7);
    prepare.parameters = parameters.data();
    prepare.parameterCount = parameters.size();
    auto candidate = host.createPreparedInstance(
        "org.talas.builtin-kernels",
        "talas.filter",
        prepare);
    auto control = host.createPreparedInstance(
        "org.talas.builtin-kernels",
        "talas.filter",
        prepare);
    require(candidate.ok() && control.ok(),
        "atomic filter candidate and control should prepare");

    const std::array overflowInput = {
        std::numeric_limits<double>::max(),
    };
    const std::array overflowSignals = {
        samples(overflowInput),
        constant(sampleRate * 0.1),
        constant(0.9),
        constant(std::numeric_limits<double>::max()),
        constant(0.0),
    };
    std::array<double, 1> rejectedOutput = {9.0};
    TalasKernelAbiOutputV1 rejected{};
    rejected.structSize = sizeof(rejected);
    rejected.samples = rejectedOutput.data();
    rejected.sampleCount = rejectedOutput.size();
    require(
        candidate.instance->process(processRecord(
            sampleRate,
            rejectedOutput.size(),
            overflowSignals,
            rejected)) == TALAS_KERNEL_ABI_STATUS_INVALID_VALUE
            && rejectedOutput == std::array<double, 1>{9.0},
        "late finite overflow must reject without exposing partial output");

    const std::array validInput = {1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    const std::array validSignals = {
        samples(validInput),
        constant(sampleRate * 0.1),
        constant(0.9),
        constant(0.5),
        constant(0.0),
    };
    std::array<double, validInput.size()> candidateOutput{};
    std::array<double, validInput.size()> controlOutput{};
    TalasKernelAbiOutputV1 candidateRecord{};
    candidateRecord.structSize = sizeof(candidateRecord);
    candidateRecord.samples = candidateOutput.data();
    candidateRecord.sampleCount = candidateOutput.size();
    TalasKernelAbiOutputV1 controlRecord = candidateRecord;
    controlRecord.samples = controlOutput.data();
    require(
        candidate.instance->process(processRecord(
            sampleRate,
            validInput.size(),
            validSignals,
            candidateRecord)) == TALAS_KERNEL_ABI_STATUS_OK
            && control.instance->process(processRecord(
                sampleRate,
                validInput.size(),
                validSignals,
                controlRecord)) == TALAS_KERNEL_ABI_STATUS_OK,
        "candidate and untouched control should recover with a valid block");
    requireEqual(
        candidateOutput,
        controlOutput,
        "late filter rejection must not advance ABI kernel state");
}

void testAdsrScalarAndTypedEventPartitionParity()
{
    constexpr std::array sampleRates = {44'100.0, 48'000.0, 96'000.0};
    constexpr std::array<std::size_t, 4> frameCounts = {1, 7, 64, 257};
    for (const auto sampleRate : sampleRates)
    {
        for (const auto frames : frameCounts)
        {
            auto services = hostServices();
            talas::runtime::KernelAbiHost host(
                talas::core::makeBuiltinNodeRegistry(),
                services);
            require(host.registerModule(
                        talas::runtime::builtinKernelAbiModule()).ok(),
                "scalar ADSR module should register");
            const std::array parameters = {
                scalarParameter("attackTime", 0.002),
                scalarParameter("decayTime", 0.003),
                scalarParameter("sustainLevel", 0.35),
                scalarParameter("releaseTime", 0.004),
                scalarParameter("scale", 0.8),
                scalarParameter("legato", 1.0),
            };
            auto prepare = prepareRecord(sampleRate, frames);
            prepare.parameters = parameters.data();
            prepare.parameterCount = parameters.size();
            auto abi = host.createPreparedInstance(
                "org.talas.builtin-kernels",
                "talas.adsr",
                prepare);
            require(abi.ok(), "scalar ABI ADSR should prepare");
            talas::runtime::AdsrKernel direct;
            require(
                direct.prepare({sampleRate})
                    == talas::runtime::KernelProcessStatus::Ok,
                "scalar direct ADSR should prepare");
            std::vector<double> gate(frames, 1.0);
            if (frames > 2)
            {
                for (std::size_t frame = frames / 3;
                     frame < (frames * 2) / 3;
                     ++frame)
                {
                    gate[frame] = 0.0;
                }
            }
            std::vector<double> directOutput(frames);
            std::vector<double> abiOutput(frames);
            talas::runtime::AdsrKernelBlock directBlock;
            directBlock.output = directOutput;
            directBlock.attackTime =
                talas::runtime::KernelSignal::constantValue(0.002);
            directBlock.decayTime =
                talas::runtime::KernelSignal::constantValue(0.003);
            directBlock.sustainLevel =
                talas::runtime::KernelSignal::constantValue(0.35);
            directBlock.releaseTime =
                talas::runtime::KernelSignal::constantValue(0.004);
            directBlock.scale =
                talas::runtime::KernelSignal::constantValue(0.8);
            directBlock.gate = talas::runtime::KernelSignal::sampleValues(gate);
            directBlock.legato =
                talas::runtime::KernelSignal::constantValue(1.0);
            require(
                direct.process(directBlock)
                    == talas::runtime::KernelProcessStatus::Ok,
                "scalar direct ADSR should process");
            const std::array inputs = {samples(gate)};
            TalasKernelAbiOutputV1 output{};
            output.structSize = sizeof(output);
            output.samples = abiOutput.data();
            output.sampleCount = frames;
            require(
                abi.instance->process(processRecord(
                    sampleRate,
                    frames,
                    inputs,
                    output)) == TALAS_KERNEL_ABI_STATUS_OK,
                "scalar ABI ADSR should process");
            requireEqual(
                abiOutput,
                directOutput,
                "scalar ADSR gate/release/legato parity should be exact");

            direct.reset();
            require(
                abi.instance->reset(resetRecord(sampleRate))
                    == TALAS_KERNEL_ABI_STATUS_OK,
                "ABI ADSR reset should succeed");
            std::fill(directOutput.begin(), directOutput.end(), 0.0);
            std::fill(abiOutput.begin(), abiOutput.end(), 0.0);
            require(
                direct.process(directBlock)
                    == talas::runtime::KernelProcessStatus::Ok,
                "reset direct ADSR should process");
            require(
                abi.instance->process(processRecord(
                    sampleRate,
                    frames,
                    inputs,
                    output)) == TALAS_KERNEL_ABI_STATUS_OK,
                "reset ABI ADSR should process");
            requireEqual(
                abiOutput,
                directOutput,
                "ABI ADSR reset should restore fresh envelope state");
        }

        constexpr std::array<std::size_t, 4> partitions = {1, 7, 64, 257};
        constexpr std::size_t totalFrames = 329;
        constexpr std::array<talas::runtime::AdsrGateEvent, 5> directEvents = {{
            {1, 0, 1, true},
            {1, 8, 2, false},
            {1, 8, 3, true},
            {1, 72, 4, false},
            {1, 328, 5, true},
        }};
        auto services = hostServices();
        talas::runtime::KernelAbiHost host(
            talas::core::makeBuiltinNodeRegistry(),
            services);
        require(host.registerModule(talas::runtime::builtinKernelAbiModule()).ok(),
            "typed ADSR module should register");
        const std::array parameters = {
            scalarParameter("attackTime", 0.002),
            scalarParameter("decayTime", 0.003),
            scalarParameter("sustainLevel", 0.35),
            scalarParameter("releaseTime", 0.004),
            scalarParameter("scale", 0.8),
            scalarParameter("legato", 1.0),
        };
        auto prepare = prepareRecord(sampleRate, partitions.back());
        prepare.parameters = parameters.data();
        prepare.parameterCount = parameters.size();
        auto abi = host.createPreparedInstance(
            "org.talas.builtin-kernels",
            "talas.adsr",
            prepare);
        require(abi.ok(), "typed ABI ADSR should prepare");
        talas::runtime::AdsrKernel direct;
        require(
            direct.prepare({sampleRate})
                == talas::runtime::KernelProcessStatus::Ok,
            "typed direct ADSR should prepare");
        std::vector<double> directOutput(totalFrames);
        std::vector<double> abiOutput(totalFrames);
        talas::runtime::AdsrKernelBlock directBlock;
        directBlock.output = directOutput;
        directBlock.attackTime =
            talas::runtime::KernelSignal::constantValue(0.002);
        directBlock.decayTime =
            talas::runtime::KernelSignal::constantValue(0.003);
        directBlock.sustainLevel =
            talas::runtime::KernelSignal::constantValue(0.35);
        directBlock.releaseTime =
            talas::runtime::KernelSignal::constantValue(0.004);
        directBlock.scale =
            talas::runtime::KernelSignal::constantValue(0.8);
        directBlock.gate =
            talas::runtime::KernelSignal::constantValue(0.0);
        directBlock.legato =
            talas::runtime::KernelSignal::constantValue(1.0);
        directBlock.typedGateEvents = directEvents;
        directBlock.typedGateTargetIndex = 1;
        directBlock.useTypedGateEvents = true;
        require(
            direct.process(directBlock)
                == talas::runtime::KernelProcessStatus::Ok,
            "whole typed direct ADSR should process");

        std::size_t offset = 0;
        for (const auto frames : partitions)
        {
            std::array<TalasKernelAbiEventV1, directEvents.size()> localEvents{};
            std::size_t localCount = 0;
            for (const auto& event : directEvents)
            {
                if (event.frameOffset < offset
                    || event.frameOffset >= offset + frames)
                {
                    continue;
                }
                auto& local = localEvents[localCount++];
                local.structSize = sizeof(local);
                local.kind = TALAS_KERNEL_ABI_EVENT_GATE;
                local.targetIndex = event.targetIndex;
                local.frameOffset = event.frameOffset - offset;
                local.sequence = event.sequence;
                local.payloadBits = event.high
                    ? TALAS_KERNEL_ABI_GATE_EVENT_HIGH
                    : 0;
            }
            const std::array inputs = {constant(0.0)};
            TalasKernelAbiOutputV1 output{};
            output.structSize = sizeof(output);
            output.samples = abiOutput.data() + offset;
            output.sampleCount = frames;
            require(
                abi.instance->process(processRecord(
                    sampleRate,
                    frames,
                    inputs,
                    output,
                    std::span<const TalasKernelAbiEventV1>(
                        localEvents.data(),
                        localCount),
                    TALAS_KERNEL_ABI_PROCESS_TYPED_EVENTS))
                    == TALAS_KERNEL_ABI_STATUS_OK,
                "partitioned typed ABI ADSR should process");
            offset += frames;
        }
        requireEqual(
            abiOutput,
            directOutput,
            "typed ADSR events should remain ordered and partition invariant");

        constexpr std::array<talas::runtime::AdsrGateEvent, 5> denseDirect = {{
            {1, 0, 1, true},
            {1, 0, 2, false},
            {1, 0, 3, true},
            {1, 0, 4, false},
            {1, 0, 5, true},
        }};
        std::array<TalasKernelAbiEventV1, denseDirect.size()> denseAbi{};
        for (std::size_t index = 0; index < denseDirect.size(); ++index)
        {
            denseAbi[index].structSize = sizeof(denseAbi[index]);
            denseAbi[index].kind = TALAS_KERNEL_ABI_EVENT_GATE;
            denseAbi[index].targetIndex = denseDirect[index].targetIndex;
            denseAbi[index].frameOffset = denseDirect[index].frameOffset;
            denseAbi[index].sequence = denseDirect[index].sequence;
            denseAbi[index].payloadBits = denseDirect[index].high
                ? TALAS_KERNEL_ABI_GATE_EVENT_HIGH
                : 0;
        }
        auto denseServices = hostServices();
        talas::runtime::KernelAbiHost denseHost(
            talas::core::makeBuiltinNodeRegistry(),
            denseServices);
        require(
            denseHost.registerModule(
                talas::runtime::builtinKernelAbiModule()).ok(),
            "dense typed ADSR module should register");
        auto densePrepare = prepareRecord(sampleRate, 1);
        densePrepare.parameters = parameters.data();
        densePrepare.parameterCount = parameters.size();
        auto denseInstance = denseHost.createPreparedInstance(
            "org.talas.builtin-kernels",
            "talas.adsr",
            densePrepare);
        require(denseInstance.ok(), "dense typed ABI ADSR should prepare");

        talas::runtime::AdsrKernel denseDirectKernel;
        require(
            denseDirectKernel.prepare({sampleRate})
                == talas::runtime::KernelProcessStatus::Ok,
            "dense typed direct ADSR should prepare");
        std::array<double, 1> denseDirectOutput{};
        talas::runtime::AdsrKernelBlock denseDirectBlock;
        denseDirectBlock.output = denseDirectOutput;
        denseDirectBlock.attackTime =
            talas::runtime::KernelSignal::constantValue(0.002);
        denseDirectBlock.decayTime =
            talas::runtime::KernelSignal::constantValue(0.003);
        denseDirectBlock.sustainLevel =
            talas::runtime::KernelSignal::constantValue(0.35);
        denseDirectBlock.releaseTime =
            talas::runtime::KernelSignal::constantValue(0.004);
        denseDirectBlock.scale =
            talas::runtime::KernelSignal::constantValue(0.8);
        denseDirectBlock.gate =
            talas::runtime::KernelSignal::constantValue(0.0);
        denseDirectBlock.legato =
            talas::runtime::KernelSignal::constantValue(1.0);
        denseDirectBlock.typedGateEvents = denseDirect;
        denseDirectBlock.typedGateTargetIndex = 1;
        denseDirectBlock.useTypedGateEvents = true;
        require(
            denseDirectKernel.process(denseDirectBlock)
                == talas::runtime::KernelProcessStatus::Ok,
            "five ordered events in one frame should be valid directly");

        std::array<double, 1> denseAbiOutput{};
        const std::array denseInputs = {constant(0.0)};
        TalasKernelAbiOutputV1 denseOutput{};
        denseOutput.structSize = sizeof(denseOutput);
        denseOutput.samples = denseAbiOutput.data();
        denseOutput.sampleCount = denseAbiOutput.size();
        require(
            denseInstance.instance->process(processRecord(
                sampleRate,
                1,
                denseInputs,
                denseOutput,
                denseAbi,
                TALAS_KERNEL_ABI_PROCESS_TYPED_EVENTS))
                == TALAS_KERNEL_ABI_STATUS_OK,
            "ABI ADSR must accept host-valid dense events independent of frame count");
        requireEqual(
            denseAbiOutput,
            denseDirectOutput,
            "dense one-frame ADSR events should preserve direct/ABI parity");
    }
}

void testAdsrInvalidPreparedValuesRejectBeforePublication()
{
    constexpr double sampleRate = 48'000.0;
    auto services = hostServices();
    talas::runtime::KernelAbiHost host(
        talas::core::makeBuiltinNodeRegistry(),
        services);
    require(host.registerModule(talas::runtime::builtinKernelAbiModule()).ok(),
        "invalid fixed ADSR module should register");
    const std::array invalidCases = {
        std::pair<std::size_t, double>{0, 0.0},
        std::pair<std::size_t, double>{1, -0.1},
        std::pair<std::size_t, double>{2, 1.1},
        std::pair<std::size_t, double>{3, 0.0},
    };
    for (const auto [index, invalidValue] : invalidCases)
    {
        std::array parameters = {
            scalarParameter("attackTime", 0.01),
            scalarParameter("decayTime", 0.1),
            scalarParameter("sustainLevel", 0.7),
            scalarParameter("releaseTime", 0.25),
            scalarParameter("scale", 1.0),
            scalarParameter("legato", 0.0),
        };
        parameters[index].scalarValue = invalidValue;
        auto prepare = prepareRecord(sampleRate, 64);
        prepare.parameters = parameters.data();
        prepare.parameterCount = parameters.size();
        const auto rejected = host.createPreparedInstance(
            "org.talas.builtin-kernels",
            "talas.adsr",
            prepare);
        require(
            !rejected.ok()
                && rejected.status
                    == TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION,
            "invalid fixed ADSR values must reject before publication");
    }

    const std::array overflowParameters = {
        scalarParameter(
            "attackTime", std::numeric_limits<double>::max()),
        scalarParameter(
            "decayTime", std::numeric_limits<double>::max()),
        scalarParameter("sustainLevel", 0.7),
        scalarParameter("releaseTime", 0.25),
        scalarParameter("scale", 1.0),
        scalarParameter("legato", 0.0),
    };
    auto overflowPrepare = prepareRecord(sampleRate, 64);
    overflowPrepare.parameters = overflowParameters.data();
    overflowPrepare.parameterCount = overflowParameters.size();
    const auto overflow = host.createPreparedInstance(
        "org.talas.builtin-kernels",
        "talas.adsr",
        overflowPrepare);
    require(!overflow.ok()
            && overflow.status
                == TALAS_KERNEL_ABI_STATUS_INVALID_CONFIGURATION,
        "ADSR ABI should reject a non-finite fixed attack-plus-decay boundary before publication");
}
}

int main()
{
    testBuiltinOfflineCheckpointContinuation();
    testStaticModuleMetadataAndOscillatorParity();
    testOscillatorPartitionResetAndModulationParity();
    testFilterModesOrdersStateAndRejectionParity();
    testFilterPartitionAndFeedbackParity();
    testFilterLateFailureIsOutputAndStateAtomic();
    testAdsrScalarAndTypedEventPartitionParity();
    testAdsrInvalidPreparedValuesRejectBeforePublication();
    return 0;
}
