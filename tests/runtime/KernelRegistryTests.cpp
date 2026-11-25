#include "talas/core/BuiltinNodeBindings.h"
#include "talas/core/BuiltinNodes.h"
#include "talas/runtime/KernelRegistry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace
{
using talas::runtime::AdsrKernel;
using talas::runtime::AdsrKernelBlock;
using talas::runtime::ClockDividerKernel;
using talas::runtime::ClockDividerKernelBlock;
using talas::runtime::ClockKernel;
using talas::runtime::ClockKernelBlock;
using talas::runtime::FilterKernel;
using talas::runtime::FilterKernelBlock;
using talas::runtime::FilterKernelConfig;
using talas::runtime::FilterKernelMode;
using talas::runtime::KernelKind;
using talas::runtime::KernelProcessStatus;
using talas::runtime::KernelProcessContext;
using talas::runtime::KernelSignal;
using talas::runtime::LogicKernel;
using talas::runtime::LogicKernelBlock;
using talas::runtime::LogicOperation;
using talas::runtime::OscillatorKernel;
using talas::runtime::OscillatorKernelBlock;
using talas::runtime::OscillatorKernelConfig;
using talas::runtime::OscillatorWavetableInterpolation;
using talas::runtime::QuantizerKernel;
using talas::runtime::QuantizerKernelBlock;
using talas::runtime::SchmittGateKernel;
using talas::runtime::SchmittGateKernelBlock;
using talas::runtime::SlewKernel;
using talas::runtime::SlewKernelBlock;

void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(double actual, double expected, const std::string& message)
{
    if (std::abs(actual - expected) > 1.0e-12)
    {
        throw std::runtime_error(
            message + ": expected " + std::to_string(expected)
            + ", got " + std::to_string(actual));
    }
}

KernelSignal samples(std::span<const double> values)
{
    return KernelSignal::sampleValues(values);
}

void testRegistryIsSparseAndExact()
{
    const auto descriptors = talas::runtime::builtinKernelDescriptors();
    require(descriptors.size() == 9, "M1 kernel registry should expose the three migrated nodes and six native utilities");

    const auto* oscillator = talas::runtime::findBuiltinKernel("talas.oscillator");
    const auto* filter = talas::runtime::findBuiltinKernel("talas.filter");
    const auto* adsr = talas::runtime::findBuiltinKernel("talas.adsr");
    require(oscillator != nullptr && oscillator->kind == KernelKind::Oscillator,
        "registry should resolve the oscillator kernel");
    require(filter != nullptr && filter->kind == KernelKind::Filter,
        "registry should resolve the filter kernel");
    require(adsr != nullptr && adsr->kind == KernelKind::Adsr,
        "registry should resolve the ADSR kernel");
    require(talas::runtime::findBuiltinKernel("talas.clock")->kind == KernelKind::Clock,
        "registry should resolve the native Clock kernel");
    require(talas::runtime::findBuiltinKernel("talas.clockDivider")->kind == KernelKind::ClockDivider,
        "registry should resolve the native Clock Divider kernel");
    require(talas::runtime::findBuiltinKernel("talas.quantizer")->kind == KernelKind::Quantizer,
        "registry should resolve the native Quantizer kernel");
    require(talas::runtime::findBuiltinKernel("talas.gate")->kind == KernelKind::Gate,
        "registry should resolve the native Gate kernel");
    require(talas::runtime::findBuiltinKernel("talas.logic")->kind == KernelKind::Logic,
        "registry should resolve the native Logic kernel");
    require(talas::runtime::findBuiltinKernel("talas.slew")->kind == KernelKind::Slew,
        "registry should resolve the native Slew kernel");
    require(talas::runtime::findBuiltinKernel("talas.vca") == nullptr,
        "unmigrated nodes should remain outside the sparse kernel registry");

    for (const auto& descriptor : talas::core::makeBuiltinNodeRegistry().nodeTypes())
    {
        const auto registered =
            talas::runtime::findBuiltinKernel(descriptor.typeId) != nullptr;
        require(
            talas::core::hasBuiltinOfflineKernelBinding(descriptor.typeId)
                    == registered
                && talas::core::hasBuiltinPreparedRealtimeKernelBinding(
                       descriptor.typeId)
                    == registered,
            descriptor.typeId
                + " should not drift between kernel registry and capability authority");
    }
}

void testClockUsesAbsoluteTransportFramesAcrossPartitions()
{
    ClockKernel whole;
    ClockKernel partitioned;
    require(whole.prepare({.sampleRate = 8.0, .frequency = 1.0, .dutyCycle = 0.5, .enabled = true})
                == KernelProcessStatus::Ok,
        "whole Clock should prepare");
    require(partitioned.prepare({.sampleRate = 8.0, .frequency = 1.0, .dutyCycle = 0.5, .enabled = true})
                == KernelProcessStatus::Ok,
        "partitioned Clock should prepare");

    std::array<double, 12> wholeOutput{};
    ClockKernelBlock wholeBlock{wholeOutput};
    require(whole.process(wholeBlock, KernelProcessContext{.transport = {
                .epoch = 1,
                .absoluteFrame = 5,
                .playheadFrame = 5,
                .sampleRate = 16.0,
                .playing = true,
            }})
                == KernelProcessStatus::Ok,
        "whole Clock should process from a nonzero transport frame");

    std::array<double, 12> partitionedOutput{};
    constexpr std::array<std::size_t, 4> partitions = {2, 1, 6, 3};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        ClockKernelBlock block{
            std::span<double>(partitionedOutput).subspan(offset, frames),
        };
        require(partitioned.process(
                    block,
                    KernelProcessContext{.transport = {
                        .epoch = 1,
                        .absoluteFrame = 5 + offset,
                        .playheadFrame = 5 + offset,
                        .sampleRate = 16.0,
                        .playing = true,
                    }})
                    == KernelProcessStatus::Ok,
            "partitioned Clock should process");
        offset += frames;
    }

    require(partitionedOutput == wholeOutput,
        "Clock output should depend on host transport frames, not callback partitions");
    constexpr std::array<double, 12> expected = {
        0.0, 0.0, 0.0, 1.0, 1.0, 1.0,
        1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
    };
    require(wholeOutput == expected,
        "Clock should expose the configured transport-aligned square wave");

    ClockKernel invalid;
    require(invalid.prepare({.sampleRate = 8.0, .frequency = 0.0, .dutyCycle = 0.5})
                == KernelProcessStatus::InvalidConfiguration,
        "Clock should reject a non-positive frequency");
}

void testDynamicClockParametersPreservePhaseAndRejectAtomically()
{
    constexpr std::array<double, 8> frequencies = {
        1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0, 2.0,
    };
    constexpr std::array<double, 8> dutyCycles = {
        0.5, 0.5, 0.5, 0.5, 1.0, 1.0, 0.0, 0.0,
    };
    constexpr std::array<double, 8> expected = {
        1.0, 1.0, 1.0, 0.0, 1.0, 1.0, 0.0, 0.0,
    };

    ClockKernel whole;
    ClockKernel partitioned;
    const auto config = talas::runtime::ClockKernelConfig{
        .sampleRate = 8.0,
        .frequency = 1.0,
        .dutyCycle = 0.5,
        .enabled = true,
        .phaseContinuous = true,
    };
    require(whole.prepare(config) == KernelProcessStatus::Ok
            && partitioned.prepare(config) == KernelProcessStatus::Ok,
        "dynamic Clock kernels should prepare");

    std::array<double, frequencies.size()> wholeOutput{};
    require(whole.process({
                .output = wholeOutput,
                .frequency = samples(frequencies),
                .dutyCycle = samples(dutyCycles),
            }, {.transport = {
                .epoch = 7,
                .absoluteFrame = 1,
                .playheadFrame = 1,
                .sampleRate = 8.0,
                .playing = true,
            }}) == KernelProcessStatus::Ok,
        "dynamic Clock should process one whole block");
    require(wholeOutput == expected,
        "frequency changes should preserve phase and duty changes should apply at their exact frame");

    std::array<double, frequencies.size()> partitionedOutput{};
    constexpr std::array<std::size_t, 4> partitions = {2, 1, 3, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        require(partitioned.process({
                    .output = std::span<double>(partitionedOutput).subspan(offset, frames),
                    .frequency = samples(std::span<const double>(frequencies).subspan(offset, frames)),
                    .dutyCycle = samples(std::span<const double>(dutyCycles).subspan(offset, frames)),
                }, {.transport = {
                    .epoch = 7,
                    .absoluteFrame = 1 + offset,
                    .playheadFrame = 1 + offset,
                    .sampleRate = 8.0,
                    .playing = true,
                }}) == KernelProcessStatus::Ok,
            "dynamic Clock should process each irregular partition");
        offset += frames;
    }
    require(partitionedOutput == wholeOutput,
        "dynamic Clock should be partition invariant");

    ClockKernel rejected;
    ClockKernel fresh;
    require(rejected.prepare(config) == KernelProcessStatus::Ok
            && fresh.prepare(config) == KernelProcessStatus::Ok,
        "transactional Clock fixtures should prepare");
    const std::array<double, 2> invalidFrequency = {
        1.0,
        std::numeric_limits<double>::quiet_NaN(),
    };
    std::array<double, 2> rejectedOutput = {7.0, 7.0};
    require(rejected.process({
                .output = rejectedOutput,
                .frequency = samples(invalidFrequency),
                .dutyCycle = KernelSignal::constantValue(0.5),
            }, {.transport = {
                .epoch = 9,
                .absoluteFrame = 3,
                .playheadFrame = 3,
                .sampleRate = 8.0,
                .playing = true,
            }}) == KernelProcessStatus::InvalidValue,
        "dynamic Clock should reject a non-finite frequency");
    require(rejectedOutput == std::array<double, 2>{7.0, 7.0},
        "dynamic Clock rejection should not partially publish output");

    const std::array<double, 2> invalidDutyCycle = {0.5, 1.25};
    rejectedOutput = {7.0, 7.0};
    require(rejected.process({
                .output = rejectedOutput,
                .frequency = KernelSignal::constantValue(1.0),
                .dutyCycle = samples(invalidDutyCycle),
            }, {.transport = {
                .epoch = 9,
                .absoluteFrame = 3,
                .playheadFrame = 3,
                .sampleRate = 8.0,
                .playing = true,
            }}) == KernelProcessStatus::InvalidValue
            && rejectedOutput == std::array<double, 2>{7.0, 7.0},
        "dynamic Clock should reject an out-of-range duty cycle transactionally");

    constexpr std::array<double, 1> truncatedFrequency = {1.0};
    rejectedOutput = {7.0, 7.0};
    require(rejected.process({
                .output = rejectedOutput,
                .frequency = samples(truncatedFrequency),
                .dutyCycle = KernelSignal::constantValue(0.5),
            }, {.transport = {
                .epoch = 9,
                .absoluteFrame = 3,
                .playheadFrame = 3,
                .sampleRate = 8.0,
                .playing = true,
            }}) == KernelProcessStatus::InvalidBlockLayout
            && rejectedOutput == std::array<double, 2>{7.0, 7.0},
        "dynamic Clock should require exact signal layouts without publishing output");

    std::array<double, 2> retriedOutput{};
    std::array<double, 2> freshOutput{};
    const auto acceptedBlock = [](std::span<double> output) {
        return ClockKernelBlock{
            .output = output,
            .frequency = KernelSignal::constantValue(2.0),
            .dutyCycle = KernelSignal::constantValue(0.5),
        };
    };
    const auto retryContext = KernelProcessContext{.transport = {
        .epoch = 9,
        .absoluteFrame = 3,
        .playheadFrame = 3,
        .sampleRate = 8.0,
        .playing = true,
    }};
    require(rejected.process(acceptedBlock(retriedOutput), retryContext)
                == KernelProcessStatus::Ok
            && fresh.process(acceptedBlock(freshOutput), retryContext)
                == KernelProcessStatus::Ok,
        "dynamic Clock should accept a corrected retry");
    require(retriedOutput == freshOutput,
        "rejected dynamic Clock blocks should not advance phase state");

    rejected.reset();
    std::array<double, 1> resetOutput{};
    std::array<double, 1> resetReference{};
    ClockKernel resetFresh;
    require(resetFresh.prepare(config) == KernelProcessStatus::Ok
            && rejected.process(acceptedBlock(resetOutput), {
                .transport = {
                    .epoch = 10,
                    .absoluteFrame = 4,
                    .playheadFrame = 4,
                    .sampleRate = 8.0,
                    .playing = true,
                }}) == KernelProcessStatus::Ok
            && resetFresh.process(acceptedBlock(resetReference), {
                .transport = {
                    .epoch = 10,
                    .absoluteFrame = 4,
                    .playheadFrame = 4,
                    .sampleRate = 8.0,
                    .playing = true,
                }}) == KernelProcessStatus::Ok,
        "reset Clock fixtures should render at the adopted transport frame");
    require(resetOutput == resetReference,
        "reset should deterministically rebase dynamic Clock phase");

    ClockKernel overflowing;
    require(overflowing.prepare({
                .sampleRate = std::numeric_limits<double>::denorm_min(),
                .frequency = 1.0,
                .dutyCycle = 0.5,
                .enabled = true,
                .phaseContinuous = true,
            }) == KernelProcessStatus::Ok,
        "overflowing dynamic Clock fixture should prepare");
    std::array<double, 1> overflowOutput = {9.0};
    require(overflowing.process({
                .output = overflowOutput,
                .frequency = KernelSignal::constantValue(
                    std::numeric_limits<double>::max()),
                .dutyCycle = KernelSignal::constantValue(0.5),
            }, {.transport = {
                .epoch = 1,
                .absoluteFrame = 1,
                .playheadFrame = 1,
                .sampleRate = std::numeric_limits<double>::denorm_min(),
                .playing = true,
            }}) == KernelProcessStatus::InvalidValue
            && overflowOutput[0] == 9.0,
        "dynamic Clock should reject a non-finite phase increment atomically");
}

void testDividerGateQuantizerLogicAndSlewArePartitionInvariant()
{
    constexpr std::array<double, 16> clock = {
        0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0,
        0.0, 1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 0.0,
    };
    std::array<double, clock.size()> steps{};
    std::array<double, clock.size()> gates{};
    ClockDividerKernel divider;
    require(divider.prepare({.division = 2, .steps = 3}) == KernelProcessStatus::Ok,
        "Clock Divider should prepare");
    ClockDividerKernelBlock dividerBlock;
    dividerBlock.stepOutput = steps;
    dividerBlock.gateOutput = gates;
    dividerBlock.clock = samples(clock);
    dividerBlock.division = KernelSignal::constantValue(2.0);
    dividerBlock.steps = KernelSignal::constantValue(3.0);
    require(divider.process(dividerBlock) == KernelProcessStatus::Ok,
        "Clock Divider should process bridged scalar edges");
    require(gates[5] == 1.0 && gates[6] == 1.0 && gates[7] == 0.0
            && gates[13] == 1.0 && gates[14] == 1.0 && gates[15] == 0.0,
        "Clock Divider should open only qualified divided gates");
    require(steps[12] == 0.0 && steps[13] == 1.0,
        "Clock Divider should hold and advance a bounded step index");

    QuantizerKernel quantizer;
    require(quantizer.prepare({.stepSize = 55.0, .offset = 220.0})
                == KernelProcessStatus::Ok,
        "Quantizer should prepare");
    constexpr std::array<double, 4> indices = {-0.6, 0.49, 1.5, 2.0};
    std::array<double, indices.size()> quantized{};
    require(quantizer.process({
                .output = quantized,
                .value = samples(indices),
                .stepSize = KernelSignal::constantValue(55.0),
                .offset = KernelSignal::constantValue(220.0),
            })
                == KernelProcessStatus::Ok,
        "Quantizer should process indices");
    require(quantized == std::array<double, 4>{165.0, 220.0, 330.0, 330.0},
        "Quantizer should map rounded indices onto its explicit grid");

    SchmittGateKernel gate;
    require(gate.prepare({.lowThreshold = 0.25, .highThreshold = 0.75})
                == KernelProcessStatus::Ok,
        "Gate should prepare");
    constexpr std::array<double, 6> gateInput = {0.0, 0.8, 0.5, 0.3, 0.1, 0.9};
    std::array<double, gateInput.size()> gateOutput{};
    require(gate.process({
                .output = gateOutput,
                .value = samples(gateInput),
                .lowThreshold = KernelSignal::constantValue(0.25),
                .highThreshold = KernelSignal::constantValue(0.75),
            })
                == KernelProcessStatus::Ok,
        "Gate should process");
    require(gateOutput == std::array<double, 6>{0.0, 1.0, 1.0, 1.0, 0.0, 1.0},
        "Gate should hold its Schmitt state between thresholds");

    LogicKernel logic;
    require(logic.prepare({.operation = LogicOperation::Xor}) == KernelProcessStatus::Ok,
        "Logic should prepare");
    constexpr std::array<double, 4> left = {0.0, 1.0, 0.0, 1.0};
    constexpr std::array<double, 4> right = {0.0, 0.0, 1.0, 1.0};
    std::array<double, 4> logicOutput{};
    require(logic.process({.output = logicOutput, .left = samples(left), .right = samples(right)})
                == KernelProcessStatus::Ok,
        "Logic should process");
    require(logicOutput == std::array<double, 4>{0.0, 1.0, 1.0, 0.0},
        "Logic should apply scalar truth semantics");

    SlewKernel wholeSlew;
    SlewKernel partitionedSlew;
    require(wholeSlew.prepare({.sampleRate = 4.0, .riseTime = 0.5, .fallTime = 0.25})
                == KernelProcessStatus::Ok,
        "whole Slew should prepare");
    require(partitionedSlew.prepare({.sampleRate = 4.0, .riseTime = 0.5, .fallTime = 0.25})
                == KernelProcessStatus::Ok,
        "partitioned Slew should prepare");
    constexpr std::array<double, 7> slewInput = {0.0, 1.0, 1.0, 1.0, 0.0, 0.0, 2.0};
    std::array<double, slewInput.size()> wholeSlewOutput{};
    std::array<double, slewInput.size()> partitionedSlewOutput{};
    require(wholeSlew.process({
                .output = wholeSlewOutput,
                .value = samples(slewInput),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.25),
            })
                == KernelProcessStatus::Ok,
        "whole Slew should process");
    constexpr std::array<std::size_t, 3> slewPartitions = {2, 3, 2};
    std::size_t offset = 0;
    for (const auto frames : slewPartitions)
    {
        require(partitionedSlew.process({
                    .output = std::span<double>(partitionedSlewOutput).subspan(offset, frames),
                    .value = samples(std::span<const double>(slewInput).subspan(offset, frames)),
                    .riseTime = KernelSignal::constantValue(0.5),
                    .fallTime = KernelSignal::constantValue(0.25),
                }) == KernelProcessStatus::Ok,
            "partitioned Slew should process");
        offset += frames;
    }
    for (std::size_t frame = 0; frame < slewInput.size(); ++frame)
        requireNear(partitionedSlewOutput[frame], wholeSlewOutput[frame],
            "Slew should preserve state across partitions");
    require(wholeSlewOutput[0] == 0.0 && wholeSlewOutput[1] > 0.0
            && wholeSlewOutput[1] < 1.0 && wholeSlewOutput[4] < wholeSlewOutput[3],
        "Slew should initialize without a hidden ramp and smooth both directions");
}

void testDynamicClockDividerParametersUseLockedModuloTransitionsAtomically()
{
    constexpr std::array<double, 18> clock = {
        0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0,
        1.0, 1.0, 0.0, 0.0, 1.0, 1.0, 1.0, 0.0, 1.0,
    };
    constexpr std::array<double, 18> division = {
        1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
        1.0, 1.0, 1.0, 1.0, 1.0, 2.0, 2.0, 2.0, 2.0,
    };
    constexpr std::array<double, 18> steps = {
        5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0, 5.0,
        5.0, 3.0, 3.0, 3.0, 3.0, 2.0, 2.0, 2.0, 2.0,
    };
    constexpr std::array<double, 18> reset = {
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 0.0, 0.0,
    };

    const auto process = [&](ClockDividerKernel& kernel,
                             std::span<double> stepOutput,
                             std::span<double> gateOutput,
                             std::size_t offset) {
        const auto frames = stepOutput.size();
        return kernel.process({
            .stepOutput = stepOutput,
            .gateOutput = gateOutput,
            .clock = samples(std::span{clock}.subspan(offset, frames)),
            .reset = samples(std::span{reset}.subspan(offset, frames)),
            .division = samples(std::span{division}.subspan(offset, frames)),
            .steps = samples(std::span{steps}.subspan(offset, frames)),
        });
    };

    ClockDividerKernel whole;
    ClockDividerKernel partitioned;
    require(whole.prepare({.division = 1, .steps = 5})
                == KernelProcessStatus::Ok
            && partitioned.prepare({.division = 1, .steps = 5})
                == KernelProcessStatus::Ok,
        "dynamic Divider transition fixtures should prepare");
    std::array<double, 18> wholeStep{};
    std::array<double, 18> wholeGate{};
    std::array<double, 18> partitionedStep{};
    std::array<double, 18> partitionedGate{};
    require(process(whole, wholeStep, wholeGate, 0)
                == KernelProcessStatus::Ok,
        "dynamic Divider should process changing integer parameters");
    std::size_t offset = 0;
    for (const auto frames : {
             std::size_t{3}, std::size_t{1}, std::size_t{6},
             std::size_t{1}, std::size_t{3}, std::size_t{4}})
    {
        require(process(
                    partitioned,
                    std::span{partitionedStep}.subspan(offset, frames),
                    std::span{partitionedGate}.subspan(offset, frames),
                    offset) == KernelProcessStatus::Ok,
            "dynamic Divider should process every irregular partition");
        offset += frames;
    }
    require(wholeStep == partitionedStep
            && wholeGate == partitionedGate,
        "dynamic Divider transitions should be partition invariant");
    require(wholeStep[9] == 4.0 && wholeGate[9] == 1.0
            && wholeStep[10] == 1.0 && wholeGate[10] == 1.0
            && wholeStep[11] == 1.0 && wholeGate[11] == 0.0,
        "steps shrink should reduce held/next state independently without closing an open gate");
    require(wholeStep[13] == 0.0 && wholeGate[13] == 0.0
            && wholeStep[14] == 0.0 && wholeGate[14] == 0.0
            && wholeStep[15] == 0.0 && wholeGate[15] == 0.0
            && wholeStep[17] == 0.0 && wholeGate[17] == 1.0,
        "simultaneous reset should win after adopting the new divisor and require two fresh edges");

    ClockDividerKernel rejected = whole;
    ClockDividerKernel reference = whole;
    constexpr std::array<double, 3> invalidDivision = {2.0, 1.5, 2.0};
    constexpr std::array<double, 3> validDivision = {2.0, 2.0, 2.0};
    constexpr std::array<double, 3> tailClock = {0.0, 1.0, 0.0};
    std::array<double, 3> rejectedStep = {7.0, 7.0, 7.0};
    std::array<double, 3> rejectedGate = {8.0, 8.0, 8.0};
    require(rejected.process({
                .stepOutput = rejectedStep,
                .gateOutput = rejectedGate,
                .clock = samples(tailClock),
                .reset = KernelSignal::constantValue(0.0),
                .division = samples(invalidDivision),
                .steps = KernelSignal::constantValue(2.0),
            }) == KernelProcessStatus::InvalidValue
            && rejectedStep == std::array<double, 3>{7.0, 7.0, 7.0}
            && rejectedGate == std::array<double, 3>{8.0, 8.0, 8.0},
        "invalid late Divider parameters should reject without output publication");
    std::array<double, 3> recoveredStep{};
    std::array<double, 3> recoveredGate{};
    std::array<double, 3> expectedStep{};
    std::array<double, 3> expectedGate{};
    require(rejected.process({
                .stepOutput = recoveredStep,
                .gateOutput = recoveredGate,
                .clock = samples(tailClock),
                .reset = KernelSignal::constantValue(0.0),
                .division = samples(validDivision),
                .steps = KernelSignal::constantValue(2.0),
            }) == KernelProcessStatus::Ok
            && reference.process({
                .stepOutput = expectedStep,
                .gateOutput = expectedGate,
                .clock = samples(tailClock),
                .reset = KernelSignal::constantValue(0.0),
                .division = samples(validDivision),
                .steps = KernelSignal::constantValue(2.0),
            }) == KernelProcessStatus::Ok
            && recoveredStep == expectedStep
            && recoveredGate == expectedGate,
        "rejected dynamic Divider blocks should preserve configuration and edge state");
}

void testDynamicQuantizerAndGateBlocksAreTransactionalAndPartitionInvariant()
{
    constexpr std::array<double, 4> indices = {0.4, 0.6, 1.4, 1.6};
    constexpr std::array<double, 4> stepSizes = {1.0, 1.0, 0.5, 0.5};
    constexpr std::array<double, 4> offsets = {0.0, 0.25, 0.0, 0.25};
    std::array<double, indices.size()> quantized{};
    QuantizerKernel quantizer;
    require(quantizer.prepare({.stepSize = 1.0, .offset = 0.0})
                == KernelProcessStatus::Ok,
        "dynamic Quantizer fixture should prepare");
    require(quantizer.process({
                .output = quantized,
                .value = samples(indices),
                .stepSize = samples(stepSizes),
                .offset = samples(offsets),
            }) == KernelProcessStatus::Ok,
        "Quantizer should accept block-level step and offset signals");
    require(quantized == std::array<double, 4>{0.0, 1.25, 0.5, 1.25},
        "Quantizer should apply each frame's effective grid");

    std::array<double, 2> rejectedQuantized = {7.0, 7.0};
    constexpr std::array<double, 2> invalidStepSizes = {1.0, 0.0};
    require(quantizer.process({
                .output = rejectedQuantized,
                .value = samples(std::span<const double>(indices).first<2>()),
                .stepSize = samples(invalidStepSizes),
                .offset = KernelSignal::constantValue(0.0),
            }) == KernelProcessStatus::InvalidValue,
        "Quantizer should reject a non-positive effective step");
    require(rejectedQuantized == std::array<double, 2>{7.0, 7.0},
        "Quantizer rejection should preserve the complete output block");

    constexpr std::array<double, 6> values = {0.0, 0.8, 0.5, 0.3, 0.1, 0.9};
    constexpr std::array<double, 6> lows = {0.25, 0.25, 0.4, 0.35, 0.15, 0.2};
    constexpr std::array<double, 6> highs = {0.75, 0.75, 0.7, 0.6, 0.8, 0.85};
    constexpr std::array<double, 6> expected = {0.0, 1.0, 1.0, 0.0, 0.0, 1.0};
    SchmittGateKernel wholeGate;
    SchmittGateKernel partitionedGate;
    require(wholeGate.prepare({}) == KernelProcessStatus::Ok
            && partitionedGate.prepare({}) == KernelProcessStatus::Ok,
        "dynamic Gate fixtures should prepare");
    std::array<double, values.size()> wholeOutput{};
    require(wholeGate.process({
                .output = wholeOutput,
                .value = samples(values),
                .lowThreshold = samples(lows),
                .highThreshold = samples(highs),
            }) == KernelProcessStatus::Ok,
        "Gate should accept block-level threshold signals");
    require(wholeOutput == expected,
        "Gate should retain hysteresis while effective thresholds change");

    std::array<double, values.size()> partitionedOutput{};
    constexpr std::array<std::size_t, 3> partitions = {2, 1, 3};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        require(partitionedGate.process({
                    .output = std::span<double>(partitionedOutput).subspan(offset, frames),
                    .value = samples(std::span<const double>(values).subspan(offset, frames)),
                    .lowThreshold = samples(std::span<const double>(lows).subspan(offset, frames)),
                    .highThreshold = samples(std::span<const double>(highs).subspan(offset, frames)),
                }) == KernelProcessStatus::Ok,
            "partitioned dynamic Gate should process");
        offset += frames;
    }
    require(partitionedOutput == wholeOutput,
        "dynamic Gate output should be callback-partition invariant");

    SchmittGateKernel transactionalGate;
    require(transactionalGate.prepare({}) == KernelProcessStatus::Ok,
        "transactional Gate fixture should prepare");
    std::array<double, 1> opened{};
    require(transactionalGate.process({
                .output = opened,
                .value = KernelSignal::constantValue(1.0),
                .lowThreshold = KernelSignal::constantValue(0.25),
                .highThreshold = KernelSignal::constantValue(0.75),
            }) == KernelProcessStatus::Ok
            && opened[0] == 1.0,
        "transactional Gate fixture should begin open");
    std::array<double, 2> rejectedGate = {8.0, 8.0};
    constexpr std::array<double, 2> rejectedValues = {0.0, 0.5};
    // A LowThreshold so high that no HighThreshold can stay above it.
    constexpr std::array<double, 2> invalidLows = {0.25, std::numeric_limits<double>::max()};
    constexpr std::array<double, 2> invalidHighs = {0.75, 0.7};
    require(transactionalGate.process({
                .output = rejectedGate,
                .value = samples(rejectedValues),
                .lowThreshold = samples(invalidLows),
                .highThreshold = samples(invalidHighs),
            }) == KernelProcessStatus::InvalidValue,
        "Gate should reject invalid effective threshold ordering");
    require(rejectedGate == std::array<double, 2>{8.0, 8.0},
        "Gate rejection should preserve the complete output block");
    std::array<double, 1> held{};
    require(transactionalGate.process({
                .output = held,
                .value = KernelSignal::constantValue(0.5),
                .lowThreshold = KernelSignal::constantValue(0.25),
                .highThreshold = KernelSignal::constantValue(0.75),
            }) == KernelProcessStatus::Ok
            && held[0] == 1.0,
        "Gate rejection should preserve hysteresis state for corrected retry");
}

void testDynamicSlewTimesAreTransactionalAndPartitionInvariant()
{
    constexpr std::array<double, 6> targets = {0.0, 1.0, 1.0, 0.0, 0.0, 2.0};
    constexpr std::array<double, 6> riseTimes = {0.5, 0.5, 0.0, 0.5, 0.5, 0.25};
    constexpr std::array<double, 6> fallTimes = {0.25, 0.25, 0.25, 0.25, 0.0, 0.25};
    SlewKernel whole;
    SlewKernel partitioned;
    require(whole.prepare({.sampleRate = 4.0, .riseTime = 0.5, .fallTime = 0.25})
                == KernelProcessStatus::Ok
            && partitioned.prepare({.sampleRate = 4.0, .riseTime = 0.5, .fallTime = 0.25})
                == KernelProcessStatus::Ok,
        "dynamic Slew fixtures should prepare");

    std::array<double, targets.size()> wholeOutput{};
    std::array<double, targets.size()> partitionedOutput{};
    require(whole.process({
                .output = wholeOutput,
                .value = samples(targets),
                .riseTime = samples(riseTimes),
                .fallTime = samples(fallTimes),
            }) == KernelProcessStatus::Ok,
        "dynamic Slew should process per-frame times");
    constexpr std::array<std::size_t, 3> partitions = {1, 3, 2};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        require(partitioned.process({
                    .output = std::span<double>(partitionedOutput).subspan(offset, frames),
                    .value = samples(std::span<const double>(targets).subspan(offset, frames)),
                    .riseTime = samples(std::span<const double>(riseTimes).subspan(offset, frames)),
                    .fallTime = samples(std::span<const double>(fallTimes).subspan(offset, frames)),
                }) == KernelProcessStatus::Ok,
            "partitioned dynamic Slew should process");
        offset += frames;
    }
    require(partitionedOutput == wholeOutput,
        "dynamic Slew should be exactly partition invariant");
    require(wholeOutput[0] == 0.0 && wholeOutput[1] > 0.0
            && wholeOutput[1] < 1.0 && wholeOutput[2] == 1.0
            && wholeOutput[3] < 1.0 && wholeOutput[3] > 0.0
            && wholeOutput[4] == 0.0 && wholeOutput[5] > 0.0
            && wholeOutput[5] < 2.0,
        "dynamic Slew should apply each effective directional time without resetting state");

    SlewKernel transactional;
    require(transactional.prepare({.sampleRate = 4.0, .riseTime = 0.5, .fallTime = 0.25})
                == KernelProcessStatus::Ok,
        "transactional Slew fixture should prepare");
    std::array<double, 2> seedOutput{};
    constexpr std::array<double, 2> seedTargets = {0.0, 1.0};
    require(transactional.process({
                .output = seedOutput,
                .value = samples(seedTargets),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.25),
            }) == KernelProcessStatus::Ok,
        "transactional Slew fixture should establish held state");
    auto reference = transactional;

    std::array<double, 2> rejectedOutput = {7.0, 7.0};
    constexpr std::array<double, 2> rejectedTargets = {2.0, 3.0};
    constexpr std::array<double, 2> invalidRiseTimes = {0.5, -1.0};
    require(transactional.process({
                .output = rejectedOutput,
                .value = samples(rejectedTargets),
                .riseTime = samples(invalidRiseTimes),
                .fallTime = KernelSignal::constantValue(0.25),
            }) == KernelProcessStatus::InvalidValue,
        "Slew should reject a negative effective time");
    require(rejectedOutput == std::array<double, 2>{7.0, 7.0},
        "Slew rejection should preserve the complete output block");

    std::array<double, 1> recovered{};
    std::array<double, 1> expected{};
    require(transactional.process({
                .output = recovered,
                .value = KernelSignal::constantValue(1.0),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.25),
            }) == KernelProcessStatus::Ok
            && reference.process({
                .output = expected,
                .value = KernelSignal::constantValue(1.0),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.25),
            }) == KernelProcessStatus::Ok
            && recovered == expected,
        "Slew rejection should preserve held state for corrected retry");

    SlewKernel overflowing;
    require(overflowing.prepare({.sampleRate = 4.0, .riseTime = 0.5, .fallTime = 0.5})
                == KernelProcessStatus::Ok,
        "overflowing Slew fixture should prepare");
    constexpr auto maximum = std::numeric_limits<double>::max();
    std::array<double, 1> negativeMaximum{};
    require(overflowing.process({
                .output = negativeMaximum,
                .value = KernelSignal::constantValue(-maximum),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.5),
            }) == KernelProcessStatus::Ok
            && negativeMaximum[0] == -maximum,
        "overflowing Slew fixture should establish an extreme finite state");
    auto overflowReference = overflowing;
    std::array<double, 1> rejectedOverflow = {9.0};
    require(overflowing.process({
                .output = rejectedOverflow,
                .value = KernelSignal::constantValue(maximum),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.5),
            }) == KernelProcessStatus::InvalidValue
            && rejectedOverflow[0] == 9.0,
        "Slew should reject a non-finite candidate without publishing output");
    std::array<double, 1> overflowRecovered{};
    std::array<double, 1> overflowExpected{};
    require(overflowing.process({
                .output = overflowRecovered,
                .value = KernelSignal::constantValue(-maximum),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.5),
            }) == KernelProcessStatus::Ok
            && overflowReference.process({
                .output = overflowExpected,
                .value = KernelSignal::constantValue(-maximum),
                .riseTime = KernelSignal::constantValue(0.5),
                .fallTime = KernelSignal::constantValue(0.5),
            }) == KernelProcessStatus::Ok
            && overflowRecovered == overflowExpected,
        "Slew candidate overflow should preserve held state for corrected retry");
}

void testUtilityRejectionDoesNotMutateStateOrOutput()
{
    ClockDividerKernel divider;
    require(divider.prepare({.division = 1, .steps = 4}) == KernelProcessStatus::Ok,
        "divider rejection fixture should prepare");
    std::array<double, 2> rejectedStep = {7.0, 7.0};
    std::array<double, 2> rejectedGate = {8.0, 8.0};
    const std::array<double, 2> invalidClock = {
        0.0,
        std::numeric_limits<double>::quiet_NaN(),
    };
    require(divider.process({
                .stepOutput = rejectedStep,
                .gateOutput = rejectedGate,
                .clock = samples(invalidClock),
                .division = KernelSignal::constantValue(1.0),
                .steps = KernelSignal::constantValue(4.0),
            }) == KernelProcessStatus::InvalidValue,
        "Divider should reject a non-finite clock transactionally");
    require(rejectedStep == std::array<double, 2>{7.0, 7.0}
            && rejectedGate == std::array<double, 2>{8.0, 8.0},
        "Divider rejection should not partially write outputs");

    constexpr std::array<double, 2> validClock = {0.0, 1.0};
    std::array<double, 2> step{};
    std::array<double, 2> gate{};
    require(divider.process({
                .stepOutput = step,
                .gateOutput = gate,
                .clock = samples(validClock),
                .division = KernelSignal::constantValue(1.0),
                .steps = KernelSignal::constantValue(4.0),
            }) == KernelProcessStatus::Ok,
        "Divider should remain usable after rejection");
    require(step[1] == 0.0 && gate[1] == 1.0,
        "rejected Divider blocks should not advance edge state");
}

void testOscillatorPreservesPhaseAcrossIrregularBlocks()
{
    OscillatorKernel kernel;
    require(
        kernel.prepare(OscillatorKernelConfig{.sampleRate = 8.0})
            == KernelProcessStatus::Ok,
        "oscillator kernel should prepare");

    std::array<double, 8> output{};
    constexpr std::array<std::size_t, 3> partitions = {3, 1, 4};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        OscillatorKernelBlock block;
        block.output = std::span<double>(output).subspan(offset, frames);
        block.frequency = KernelSignal::constantValue(1.0);
        block.amplitude = KernelSignal::constantValue(0.5);
        require(kernel.process(block) == KernelProcessStatus::Ok,
            "oscillator kernel should process each partition");
        offset += frames;
    }

    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        requireNear(
            output[frame],
            0.5 * std::sin(2.0 * pi * static_cast<double>(frame) / 8.0),
            "oscillator phase should be continuous at frame " + std::to_string(frame));
    }
}

// A nonzero Reset, negative too, resets the phase before the current sample
//.
void testOscillatorNonzeroResetRestartsBeforeCurrentSample()
{
    OscillatorKernel kernel;
    require(
        kernel.prepare(OscillatorKernelConfig{.sampleRate = 8.0})
            == KernelProcessStatus::Ok,
        "C95 reset-semantics oscillator should prepare");

    constexpr std::array<double, 5> reset = {0.0, -1.0, 1.0, 0.0, 0.0};
    std::array<double, reset.size()> output{};
    OscillatorKernelBlock block;
    block.output = output;
    block.frequency = KernelSignal::constantValue(1.0);
    block.reset = samples(reset);
    require(kernel.process(block) == KernelProcessStatus::Ok,
        "C95 reset-semantics oscillator should process");
    constexpr double rootHalf = 0.70710678118654752440084436210485;
    constexpr std::array<double, reset.size()> expected = {
        0.0, 0.0, 0.0, rootHalf, 1.0};
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        requireNear(output[frame], expected[frame],
            "a nonzero reset should restart phase before frame "
                + std::to_string(frame));
    }
}

void testOscillatorComposesFrequencyModulationAndPitchBend()
{
    OscillatorKernel kernel;
    require(
        kernel.prepare({
            .sampleRate = 8.0,
            .frequencyModulation = true,
            .maxModulationIndex = 0.5,
        }) == KernelProcessStatus::Ok,
        "C160 composed-FM oscillator should prepare");
    std::array<double, 4> output{};
    OscillatorKernelBlock block;
    block.output = output;
    block.frequency = KernelSignal::constantValue(1.0);
    block.modulator = KernelSignal::constantValue(0.5);
    block.pitchBend = KernelSignal::constantValue(0.5);
    require(kernel.process(block) == KernelProcessStatus::Ok,
        "C160 bounded FM should compose pitch-bend");
    constexpr double pi = 3.1415926535897932384626433832795;
    constexpr double effectiveFrequency = 1.875;
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        requireNear(output[frame],
            std::sin(2.0 * pi * effectiveFrequency
                * static_cast<double>(frame) / 8.0),
            "C160 composed FM-and-pitch-bend frame "
                + std::to_string(frame));
    }
}

// A modulated frequency past Nyquist was rejected, so a block of FM whose
// deviation crossed it failed the whole render (2026-11-25). The phase
// now wraps, as it does for an unmodulated frequency past Nyquist: 4.5 Hz
// at 8 Hz plays the 3.5 Hz alias turned over. Only a non-finite modulated
// frequency is rejected.
void testOscillatorFrequencyModulationPastNyquistWrapsThePhase()
{
    OscillatorKernel kernel;
    require(
        kernel.prepare({
            .sampleRate = 8.0,
            .frequencyModulation = true,
            .maxModulationIndex = 1.0,
        }) == KernelProcessStatus::Ok,
        "past-Nyquist FM oscillator should prepare");
    std::array<double, 16> output{};
    OscillatorKernelBlock block;
    block.output = output;
    block.frequency = KernelSignal::constantValue(3.0);
    block.modulator = KernelSignal::constantValue(0.5);
    require(kernel.process(block) == KernelProcessStatus::Ok,
        "FM past Nyquist should wrap the phase instead of failing");
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto time = static_cast<double>(frame) / 8.0;
        requireNear(output[frame], std::sin(2.0 * pi * 4.5 * time),
            "past-Nyquist FM frame " + std::to_string(frame));
        requireNear(output[frame], -std::sin(2.0 * pi * 3.5 * time),
            "past-Nyquist FM should alias to 3.5 Hz turned over, frame "
                + std::to_string(frame));
    }

    // Through zero and past the negative Nyquist as well.
    block.modulator = KernelSignal::constantValue(-3.0);
    require(kernel.process(block) == KernelProcessStatus::Ok,
        "FM past the negative Nyquist should wrap the phase too");

    std::array<double, 2> rejected{95.0, 95.0};
    OscillatorKernelBlock overflowing;
    overflowing.output = rejected;
    overflowing.frequency = KernelSignal::constantValue(
        std::numeric_limits<double>::max());
    overflowing.modulator = KernelSignal::constantValue(2.0);
    require(kernel.process(overflowing) == KernelProcessStatus::InvalidValue
            && rejected == std::array<double, 2>{95.0, 95.0},
        "an infinite modulated frequency should still be rejected untouched");
}

void testOscillatorPreservesLinearAndNearestCyclicWavetableSampling()
{
    constexpr std::array<double, 4> table = {1.0, 2.0, 4.0, 8.0};
    constexpr std::array<double, 8> linearExpected = {
        1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 4.5};
    constexpr std::array<double, 8> nearestExpected = {
        1.0, 2.0, 2.0, 4.0, 4.0, 8.0, 8.0, 1.0};
    for (const auto& [interpolation, expected] : {
             std::pair{OscillatorWavetableInterpolation::Linear,
                 linearExpected},
             std::pair{OscillatorWavetableInterpolation::Nearest,
                 nearestExpected},
         })
    {
        OscillatorKernel kernel;
        require(kernel.prepare({
                    .sampleRate = 8.0,
                    .wavetable = table,
                    .interpolation = interpolation,
                }) == KernelProcessStatus::Ok,
            "C95 cyclic wavetable oscillator should prepare");
        std::array<double, 8> output{};
        OscillatorKernelBlock block;
        block.output = output;
        block.frequency = KernelSignal::constantValue(1.0);
        require(kernel.process(block) == KernelProcessStatus::Ok,
            "C95 cyclic wavetable oscillator should process");
        for (std::size_t frame = 0; frame < output.size(); ++frame)
        {
            requireNear(output[frame], expected[frame],
                "C95 cyclic wavetable interpolation frame "
                    + std::to_string(frame));
        }
    }
}

void testOscillatorRejectsUnknownWavetableInterpolation()
{
    OscillatorKernel kernel;
    require(kernel.prepare({
                .sampleRate = 8.0,
                .interpolation =
                    static_cast<OscillatorWavetableInterpolation>(2),
            }) == KernelProcessStatus::InvalidConfiguration,
        "C95 Oscillator should reject unknown wavetable interpolation authority");

    std::array<double, 1> output{95.0};
    OscillatorKernelBlock block;
    block.output = output;
    require(kernel.process(block) == KernelProcessStatus::NotPrepared
            && output[0] == 95.0,
        "C95 rejected interpolation should leave the kernel unprepared and caller output untouched");
}

void testRejectedSignalBlockDoesNotAdvanceKernelState()
{
    OscillatorKernel kernel;
    require(
        kernel.prepare(OscillatorKernelConfig{.sampleRate = 8.0})
            == KernelProcessStatus::Ok,
        "rejection fixture should prepare");

    std::array<double, 2> rejectedOutput = {7.0, 7.0};
    const std::array<double, 0> truncatedFrequency = {};
    OscillatorKernelBlock rejected;
    rejected.output = rejectedOutput;
    rejected.frequency = samples(truncatedFrequency);
    require(
        kernel.process(rejected) == KernelProcessStatus::InvalidBlockLayout,
        "truncated per-frame signals should be rejected as layout errors");
    require(
        rejectedOutput == std::array<double, 2>{7.0, 7.0},
        "layout rejection should not write partial output");

    std::array<double, 1> acceptedOutput{};
    OscillatorKernelBlock accepted;
    accepted.output = acceptedOutput;
    accepted.frequency = KernelSignal::constantValue(1.0);
    require(kernel.process(accepted) == KernelProcessStatus::Ok,
        "kernel should remain usable after rejection");
    requireNear(acceptedOutput.front(), 0.0,
        "rejected blocks should not advance oscillator state");
}

void testKernelRequiresExactSignalLayoutAndIgnoresInactiveModulator()
{
    OscillatorKernel kernel;
    require(
        kernel.prepare(OscillatorKernelConfig{.sampleRate = 8.0})
            == KernelProcessStatus::Ok,
        "exact-layout fixture should prepare");

    std::array<double, 2> output{};
    constexpr std::array<double, 3> oversizedFrequency = {1.0, 1.0, 1.0};
    OscillatorKernelBlock oversized;
    oversized.output = output;
    oversized.frequency = samples(oversizedFrequency);
    require(
        kernel.process(oversized) == KernelProcessStatus::InvalidBlockLayout,
        "per-frame signals should expose exactly the processed frame count");

    std::array<double, 1> acceptedOutput{};
    constexpr std::array<double, 1> inactiveModulator = {
        std::numeric_limits<double>::quiet_NaN(),
    };
    OscillatorKernelBlock inactive;
    inactive.output = acceptedOutput;
    inactive.frequency = KernelSignal::constantValue(1.0);
    inactive.modulator = samples(inactiveModulator);
    require(kernel.process(inactive) == KernelProcessStatus::Ok,
        "disabled frequency modulation should not inspect the modulator signal");
}

void testOscillatorRejectsDerivedOverflowWithoutPublishingOutputOrState()
{
    constexpr auto sentinel = 37.0;
    const auto acceptedBlock = [](std::span<double> output) {
        OscillatorKernelBlock block;
        block.output = output;
        block.frequency = KernelSignal::constantValue(1.0);
        block.amplitude = KernelSignal::constantValue(1.0);
        block.envelope = KernelSignal::constantValue(1.0);
        return block;
    };

    OscillatorKernel outputOverflow;
    OscillatorKernel freshOutputReference;
    require(outputOverflow.prepare({.sampleRate = 8.0})
                == KernelProcessStatus::Ok
            && freshOutputReference.prepare({.sampleRate = 8.0})
                == KernelProcessStatus::Ok,
        "C95 oscillator output-overflow fixtures should prepare");
    std::array<double, 2> rejectedOutput{sentinel, sentinel};
    auto rejectedOutputBlock = acceptedBlock(rejectedOutput);
    rejectedOutputBlock.amplitude = KernelSignal::constantValue(
        std::numeric_limits<double>::max());
    rejectedOutputBlock.envelope = KernelSignal::constantValue(2.0);
    require(outputOverflow.process(rejectedOutputBlock)
                == KernelProcessStatus::InvalidValue,
        "C95 oscillator should reject a non-finite output product");
    require(rejectedOutput == std::array<double, 2>{sentinel, sentinel},
        "C95 rejected output product should preserve caller output exactly");

    std::array<double, 3> outputRetry{};
    std::array<double, 3> outputReference{};
    require(outputOverflow.process(acceptedBlock(outputRetry))
                == KernelProcessStatus::Ok
            && freshOutputReference.process(acceptedBlock(outputReference))
                == KernelProcessStatus::Ok,
        "C95 oscillator output-overflow retry fixtures should render");
    require(outputRetry == outputReference,
        "C95 rejected output product should consume no oscillator phase");

    OscillatorKernel phaseOverflow;
    OscillatorKernel freshPhaseReference;
    const auto minimumPositive = std::numeric_limits<double>::denorm_min();
    require(phaseOverflow.prepare({.sampleRate = minimumPositive})
                == KernelProcessStatus::Ok
            && freshPhaseReference.prepare({.sampleRate = minimumPositive})
                == KernelProcessStatus::Ok,
        "C95 oscillator phase-overflow fixtures should prepare");
    std::array<double, 1> rejectedPhaseOutput{sentinel};
    auto rejectedPhaseBlock = acceptedBlock(rejectedPhaseOutput);
    rejectedPhaseBlock.frequency = KernelSignal::constantValue(
        std::numeric_limits<double>::max());
    require(phaseOverflow.process(rejectedPhaseBlock)
                == KernelProcessStatus::InvalidValue,
        "C95 oscillator should reject a non-finite phase increment");
    require(rejectedPhaseOutput[0] == sentinel,
        "C95 rejected phase increment should preserve caller output exactly");

    std::array<double, 2> phaseRetry{};
    std::array<double, 2> phaseReference{};
    auto phaseRetryBlock = acceptedBlock(phaseRetry);
    auto phaseReferenceBlock = acceptedBlock(phaseReference);
    phaseRetryBlock.frequency = KernelSignal::constantValue(0.0);
    phaseReferenceBlock.frequency = KernelSignal::constantValue(0.0);
    require(phaseOverflow.process(phaseRetryBlock)
                == KernelProcessStatus::Ok
            && freshPhaseReference.process(phaseReferenceBlock)
                == KernelProcessStatus::Ok,
        "C95 oscillator phase-overflow retry fixtures should render");
    require(phaseRetry == phaseReference,
        "C95 rejected phase increment should consume no oscillator phase");

    OscillatorKernel laterFailure;
    OscillatorKernel freshLaterReference;
    require(laterFailure.prepare({.sampleRate = 8.0})
                == KernelProcessStatus::Ok
            && freshLaterReference.prepare({.sampleRate = 8.0})
                == KernelProcessStatus::Ok,
        "C95 later-frame failure fixtures should prepare");
    constexpr std::array<double, 2> laterFrequency = {
        1.0, std::numeric_limits<double>::max()};
    std::array<double, 2> laterRejected{sentinel, sentinel};
    auto laterRejectedBlock = acceptedBlock(laterRejected);
    laterRejectedBlock.frequency = samples(laterFrequency);
    require(laterFailure.process(laterRejectedBlock)
                == KernelProcessStatus::InvalidValue
            && laterRejected == std::array<double, 2>{sentinel, sentinel},
        "C95 later-frame rejection should preserve the complete output block");
    std::array<double, 1> laterRetry{};
    std::array<double, 1> laterReference{};
    require(laterFailure.process(acceptedBlock(laterRetry))
                == KernelProcessStatus::Ok
            && freshLaterReference.process(acceptedBlock(laterReference))
                == KernelProcessStatus::Ok
            && laterRetry == laterReference,
        "C95 later-frame rejection should preserve the pre-block phase");
}

void testFilterStateIsPartitionInvariant()
{
    constexpr std::array<double, 12> input = {
        1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
        0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
    };
    std::array<double, input.size()> wholeOutput{};
    std::array<double, input.size()> partitionedOutput{};

    const FilterKernelConfig config{
        .sampleRate = 1000.0,
        .mode = FilterKernelMode::LowPass,
        .stageCount = 4,
    };
    FilterKernel whole;
    FilterKernel partitioned;
    require(whole.prepare(config) == KernelProcessStatus::Ok,
        "whole-block filter should prepare");
    require(partitioned.prepare(config) == KernelProcessStatus::Ok,
        "partitioned filter should prepare");

    FilterKernelBlock wholeBlock;
    wholeBlock.input = input;
    wholeBlock.output = wholeOutput;
    wholeBlock.frequency = KernelSignal::constantValue(150.0);
    wholeBlock.q = KernelSignal::constantValue(1.0);
    require(whole.process(wholeBlock) == KernelProcessStatus::Ok,
        "whole-block filter should process");

    constexpr std::array<std::size_t, 4> partitions = {2, 5, 1, 4};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        FilterKernelBlock block;
        block.input = std::span<const double>(input).subspan(offset, frames);
        block.output = std::span<double>(partitionedOutput).subspan(offset, frames);
        block.frequency = KernelSignal::constantValue(150.0);
        block.q = KernelSignal::constantValue(1.0);
        require(partitioned.process(block) == KernelProcessStatus::Ok,
            "partitioned filter should process");
        offset += frames;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        requireNear(partitionedOutput[frame], wholeOutput[frame],
            "filter state should be partition invariant at frame " + std::to_string(frame));
    }
}

void testFilterRejectsNonFiniteIntegratorStateBeforePublication()
{
    FilterKernel rejected;
    FilterKernel fresh;
    const FilterKernelConfig config{
        .sampleRate = 8.0,
        .mode = FilterKernelMode::LowPass,
        .stageCount = 1,
    };
    require(rejected.prepare(config) == KernelProcessStatus::Ok
            && fresh.prepare(config) == KernelProcessStatus::Ok,
        "filter integrator-overflow fixtures should prepare");

    constexpr auto sentinel = 92.0;
    const std::array extremeInput{
        std::numeric_limits<double>::max(),
    };
    std::array<double, 1> rejectedOutput{sentinel};
    FilterKernelBlock rejectedBlock;
    rejectedBlock.input = extremeInput;
    rejectedBlock.output = rejectedOutput;
    rejectedBlock.frequency = KernelSignal::constantValue(
        std::nextafter(4.0, 0.0));
    rejectedBlock.q = KernelSignal::constantValue(1.0);
    require(rejected.process(rejectedBlock)
                == KernelProcessStatus::InvalidValue
            && rejectedOutput[0] == sentinel,
        "filter should reject a non-finite integrator candidate before output publication");

    const std::array safeInput{1.0, 0.0, 0.0, 0.0};
    std::array<double, safeInput.size()> retry{};
    std::array<double, safeInput.size()> expected{};
    const auto safeBlock = [&](std::span<double> output) {
        FilterKernelBlock block;
        block.input = safeInput;
        block.output = output;
        block.frequency = KernelSignal::constantValue(1.0);
        block.q = KernelSignal::constantValue(1.0);
        return block;
    };
    require(rejected.process(safeBlock(retry)) == KernelProcessStatus::Ok
            && fresh.process(safeBlock(expected)) == KernelProcessStatus::Ok
            && retry == expected,
        "rejected filter integrator overflow should consume no kernel state");
}

void testAdsrGateEdgesArePartitionInvariant()
{
    constexpr std::array<double, 12> gate = {
        1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
        0.0, 0.0, 0.0, 1.0, 1.0, 1.0,
    };
    std::array<double, gate.size()> wholeOutput{};
    std::array<double, gate.size()> partitionedOutput{};

    AdsrKernel whole;
    AdsrKernel partitioned;
    require(whole.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok,
        "whole-block ADSR should prepare");
    require(partitioned.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok,
        "partitioned ADSR should prepare");

    auto makeBlock = [&](std::span<double> output, std::span<const double> gates) {
        AdsrKernelBlock block;
        block.output = output;
        block.attackTime = KernelSignal::constantValue(0.004);
        block.decayTime = KernelSignal::constantValue(0.004);
        block.sustainLevel = KernelSignal::constantValue(0.25);
        block.releaseTime = KernelSignal::constantValue(0.004);
        block.gate = samples(gates);
        return block;
    };

    require(whole.process(makeBlock(wholeOutput, gate)) == KernelProcessStatus::Ok,
        "whole-block ADSR should process");

    constexpr std::array<std::size_t, 4> partitions = {3, 4, 2, 3};
    std::size_t offset = 0;
    for (const auto frames : partitions)
    {
        auto block = makeBlock(
            std::span<double>(partitionedOutput).subspan(offset, frames),
            std::span<const double>(gate).subspan(offset, frames));
        require(partitioned.process(block) == KernelProcessStatus::Ok,
            "partitioned ADSR should process");
        offset += frames;
    }

    for (std::size_t frame = 0; frame < gate.size(); ++frame)
    {
        requireNear(partitionedOutput[frame], wholeOutput[frame],
            "ADSR state should be partition invariant at frame " + std::to_string(frame));
    }
    require(wholeOutput[0] == 0.0, "ADSR should begin at zero");
    requireNear(wholeOutput[3], 0.75,
        "ADSR should preserve the final pre-boundary attack sample");
    requireNear(wholeOutput[4], 1.0,
        "ADSR should preserve the exact attack endpoint sample");
    require(wholeOutput[6] > wholeOutput[7], "ADSR should release after a falling gate edge");
    require(wholeOutput[9] == 0.0, "non-legato retrigger should restart attack at zero");
}

void testAdsrConsumesTypedGateEventsDirectlyAcrossPartitionsAndReset()
{
    constexpr std::array<talas::runtime::AdsrGateEvent, 5> events = {{
        {1, 0, 1, true},
        {1, 8, 2, false},
        {1, 12, 3, true},
        {1, 24, 4, false},
        {1, 32, 5, true},
    }};
    std::array<double, 40> scalarGate{};
    bool high = false;
    std::size_t nextEvent = 0;
    for (std::size_t frame = 0; frame < scalarGate.size(); ++frame)
    {
        while (nextEvent < events.size()
               && events[nextEvent].frameOffset == frame)
        {
            high = events[nextEvent].high;
            ++nextEvent;
        }
        scalarGate[frame] = high ? 1.0 : 0.0;
    }

    const auto makeBlock = [](
        std::span<double> output,
        KernelSignal gate,
        std::span<const talas::runtime::AdsrGateEvent> typedEvents = {}) {
        AdsrKernelBlock block;
        block.output = output;
        block.attackTime = KernelSignal::constantValue(0.004);
        block.decayTime = KernelSignal::constantValue(0.004);
        block.sustainLevel = KernelSignal::constantValue(0.25);
        block.releaseTime = KernelSignal::constantValue(0.004);
        block.gate = gate;
        block.typedGateEvents = typedEvents;
        block.typedGateTargetIndex = 1;
        block.useTypedGateEvents = !typedEvents.empty();
        return block;
    };

    AdsrKernel scalar;
    AdsrKernel direct;
    AdsrKernel partitioned;
    require(scalar.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok
            && direct.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok
            && partitioned.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok,
        "typed ADSR conformance kernels should prepare");
    std::array<double, 40> scalarOutput{};
    std::array<double, 40> directOutput{};
    std::array<double, 40> partitionedOutput{};
    require(scalar.process(makeBlock(
                scalarOutput,
                samples(scalarGate))) == KernelProcessStatus::Ok,
        "scalar ADSR reference should render");
    require(direct.process(makeBlock(
                directOutput,
                KernelSignal::constantValue(0.0),
                events)) == KernelProcessStatus::Ok,
        "ADSR should consume typed gate edges directly");

    constexpr std::array<std::size_t, 6> partitions = {3, 7, 1, 9, 4, 16};
    std::size_t partitionStart = 0;
    for (const auto frames : partitions)
    {
        std::array<talas::runtime::AdsrGateEvent, events.size()> localEvents{};
        std::size_t localCount = 0;
        for (const auto& event : events)
        {
            if (event.frameOffset < partitionStart
                || event.frameOffset >= partitionStart + frames)
            {
                continue;
            }
            localEvents[localCount] = event;
            localEvents[localCount].frameOffset -= partitionStart;
            ++localCount;
        }
        auto block = makeBlock(
            std::span<double>(partitionedOutput).subspan(
                partitionStart,
                frames),
            KernelSignal::constantValue(0.0),
            std::span<const talas::runtime::AdsrGateEvent>(
                localEvents.data(),
                localCount));
        block.useTypedGateEvents = true;
        require(partitioned.process(block) == KernelProcessStatus::Ok,
            "partitioned typed ADSR should process");
        partitionStart += frames;
    }
    require(scalarOutput == directOutput
            && directOutput == partitionedOutput,
        "typed ADSR gates should match scalar semantics across {3,7,1,9,4,16}");

    AdsrKernel ignoredTarget;
    require(ignoredTarget.prepare({.sampleRate = 1000.0})
                == KernelProcessStatus::Ok,
        "non-target ADSR fixture should prepare");
    constexpr std::array<talas::runtime::AdsrGateEvent, 1>
        nonTargetEvent = {{{2, 0, 1, true}}};
    std::array<double, 4> nonTargetOutput{};
    require(ignoredTarget.process(makeBlock(
                nonTargetOutput,
                KernelSignal::constantValue(1.0),
                nonTargetEvent)) == KernelProcessStatus::Ok
            && nonTargetOutput == std::array<double, 4>{},
        "typed ADSR events for another target must remain inert");

    AdsrKernel continued;
    AdsrKernel repeated;
    require(continued.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok
            && repeated.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok,
        "same-frame typed ADSR kernels should prepare");
    constexpr std::array<talas::runtime::AdsrGateEvent, 1> initial = {{
        {1, 0, 1, true},
    }};
    std::array<double, 4> continuedSeed{};
    std::array<double, 4> repeatedSeed{};
    require(continued.process(makeBlock(
                continuedSeed,
                KernelSignal::constantValue(0.0),
                initial)) == KernelProcessStatus::Ok
            && repeated.process(makeBlock(
                repeatedSeed,
                KernelSignal::constantValue(0.0),
                initial)) == KernelProcessStatus::Ok,
        "same-frame typed ADSR seed should render");
    constexpr std::array<talas::runtime::AdsrGateEvent, 2> repeatedEdges = {{
        {1, 0, 2, false},
        {1, 0, 3, true},
    }};
    std::array<double, 1> continuedOutput{};
    std::array<double, 1> repeatedOutput{};
    auto continuedBlock = makeBlock(
        continuedOutput,
        KernelSignal::constantValue(0.0));
    continuedBlock.useTypedGateEvents = true;
    require(continued.process(continuedBlock) == KernelProcessStatus::Ok
            && repeated.process(makeBlock(
                repeatedOutput,
                KernelSignal::constantValue(0.0),
                repeatedEdges)) == KernelProcessStatus::Ok,
        "same-frame typed ADSR continuations should render");
    require(repeatedOutput[0] == 0.0
            && continuedOutput[0] > repeatedOutput[0],
        "falling and rising edges at one frame should remain individually observable as a retrigger");

    repeated.reset();
    AdsrKernel fresh;
    require(fresh.prepare({.sampleRate = 1000.0}) == KernelProcessStatus::Ok,
        "fresh typed ADSR reset reference should prepare");
    std::array<double, 4> resetOutput{};
    std::array<double, 4> freshOutput{};
    require(repeated.process(makeBlock(
                resetOutput,
                KernelSignal::constantValue(0.0),
                initial)) == KernelProcessStatus::Ok
            && fresh.process(makeBlock(
                freshOutput,
                KernelSignal::constantValue(0.0),
                initial)) == KernelProcessStatus::Ok,
        "reset typed ADSR fixtures should render");
    require(resetOutput == freshOutput,
        "typed ADSR reset should restore fresh gate and envelope state");
}

void testAdsrRejectsDerivedOverflowWithoutPublishingOutputOrState()
{
    AdsrKernel rejected;
    AdsrKernel fresh;
    require(rejected.prepare({.sampleRate = 1000.0})
                == KernelProcessStatus::Ok
            && fresh.prepare({.sampleRate = 1000.0})
                == KernelProcessStatus::Ok,
        "ADSR failure-atomic fixtures should prepare");

    constexpr auto sentinel = 37.0;
    std::array<double, 3> rejectedOutput{
        sentinel,
        sentinel,
        sentinel,
    };
    constexpr std::array<double, 3> attackTimes = {
        0.004,
        std::numeric_limits<double>::max(),
        0.004,
    };
    constexpr std::array<double, 3> decayTimes = {
        0.004,
        std::numeric_limits<double>::max(),
        0.004,
    };
    AdsrKernelBlock rejectedBlock;
    rejectedBlock.output = rejectedOutput;
    rejectedBlock.attackTime = samples(attackTimes);
    rejectedBlock.decayTime = samples(decayTimes);
    rejectedBlock.sustainLevel = KernelSignal::constantValue(0.25);
    rejectedBlock.releaseTime = KernelSignal::constantValue(0.004);
    rejectedBlock.scale = KernelSignal::constantValue(1.0);
    rejectedBlock.gate = KernelSignal::constantValue(1.0);
    rejectedBlock.legato = KernelSignal::constantValue(0.0);
    require(rejected.process(rejectedBlock)
            == KernelProcessStatus::InvalidValue,
        "ADSR should reject a finite attack-plus-decay overflow");
    require(std::ranges::all_of(
                rejectedOutput,
                [](double value) { return value == sentinel; }),
        "rejected ADSR block should preserve caller output exactly");

    std::array<double, 4> retriedOutput{};
    std::array<double, 4> freshOutput{};
    const auto acceptedBlock = [](std::span<double> output) {
        AdsrKernelBlock block;
        block.output = output;
        block.attackTime = KernelSignal::constantValue(0.004);
        block.decayTime = KernelSignal::constantValue(0.004);
        block.sustainLevel = KernelSignal::constantValue(0.25);
        block.releaseTime = KernelSignal::constantValue(0.004);
        block.scale = KernelSignal::constantValue(1.0);
        block.gate = KernelSignal::constantValue(1.0);
        block.legato = KernelSignal::constantValue(0.0);
        return block;
    };
    require(rejected.process(acceptedBlock(retriedOutput))
                == KernelProcessStatus::Ok
            && fresh.process(acceptedBlock(freshOutput))
                == KernelProcessStatus::Ok,
        "ADSR should accept a corrected retry and fresh reference");
    require(retriedOutput == freshOutput,
        "rejected ADSR block should consume no envelope state");

    AdsrKernel nonFiniteScale;
    require(nonFiniteScale.prepare({.sampleRate = 1000.0})
                == KernelProcessStatus::Ok,
        "ADSR non-finite scale fixture should prepare");
    std::array<double, 1> nonFiniteScaleOutput{sentinel};
    auto nonFiniteScaleBlock = acceptedBlock(nonFiniteScaleOutput);
    nonFiniteScaleBlock.scale = KernelSignal::constantValue(
        std::numeric_limits<double>::infinity());
    require(nonFiniteScale.process(nonFiniteScaleBlock)
                == KernelProcessStatus::InvalidValue
            && nonFiniteScaleOutput[0] == sentinel,
        "ADSR should reject a non-finite scale before one-frame publication");

    AdsrKernel legato;
    require(legato.prepare({.sampleRate = 1.0})
            == KernelProcessStatus::Ok,
        "ADSR legato-overflow fixture should prepare");
    std::array<double, 2> maximumAttack{};
    auto maximumBlock = acceptedBlock(maximumAttack);
    maximumBlock.attackTime = KernelSignal::constantValue(1.0);
    maximumBlock.decayTime = KernelSignal::constantValue(1.0);
    maximumBlock.scale = KernelSignal::constantValue(
        std::numeric_limits<double>::max());
    require(legato.process(maximumBlock) == KernelProcessStatus::Ok,
        "ADSR legato-overflow fixture should establish a maximum finite value");
    std::array<double, 1> release{};
    auto releaseBlock = acceptedBlock(release);
    releaseBlock.attackTime = KernelSignal::constantValue(1.0);
    releaseBlock.decayTime = KernelSignal::constantValue(1.0);
    releaseBlock.releaseTime = KernelSignal::constantValue(2.0);
    releaseBlock.scale = KernelSignal::constantValue(
        std::numeric_limits<double>::max());
    releaseBlock.gate = KernelSignal::constantValue(0.0);
    require(legato.process(releaseBlock) == KernelProcessStatus::Ok,
        "ADSR legato-overflow fixture should capture maximum release state");
    const auto beforeEdge = legato.captureState();
    require(legato.applyGateEdge(
                true,
                1.0,
                std::numeric_limits<double>::denorm_min(),
                1.0)
            == KernelProcessStatus::InvalidValue,
        "ADSR should reject a non-finite legato reattack ratio");
    const auto afterEdge = legato.captureState();
    require(beforeEdge.elapsedTime == afterEdge.elapsedTime
            && beforeEdge.currentValue == afterEdge.currentValue
            && beforeEdge.gateWasPositive == afterEdge.gateWasPositive
            && beforeEdge.releaseElapsedTime
                == afterEdge.releaseElapsedTime
            && beforeEdge.releaseStartValue
                == afterEdge.releaseStartValue,
        "rejected ADSR gate edge should preserve every state field");
}
}

int main()
{
    testRegistryIsSparseAndExact();
    testClockUsesAbsoluteTransportFramesAcrossPartitions();
    testDynamicClockParametersPreservePhaseAndRejectAtomically();
    testDividerGateQuantizerLogicAndSlewArePartitionInvariant();
    testDynamicClockDividerParametersUseLockedModuloTransitionsAtomically();
    testDynamicQuantizerAndGateBlocksAreTransactionalAndPartitionInvariant();
    testDynamicSlewTimesAreTransactionalAndPartitionInvariant();
    testUtilityRejectionDoesNotMutateStateOrOutput();
    testOscillatorPreservesPhaseAcrossIrregularBlocks();
    testOscillatorNonzeroResetRestartsBeforeCurrentSample();
    testOscillatorComposesFrequencyModulationAndPitchBend();
    testOscillatorFrequencyModulationPastNyquistWrapsThePhase();
    testOscillatorPreservesLinearAndNearestCyclicWavetableSampling();
    testOscillatorRejectsUnknownWavetableInterpolation();
    testRejectedSignalBlockDoesNotAdvanceKernelState();
    testKernelRequiresExactSignalLayoutAndIgnoresInactiveModulator();
    testOscillatorRejectsDerivedOverflowWithoutPublishingOutputOrState();
    testFilterStateIsPartitionInvariant();
    testFilterRejectsNonFiniteIntegratorStateBeforePublication();
    testAdsrGateEdgesArePartitionInvariant();
    testAdsrConsumesTypedGateEventsDirectlyAcrossPartitionsAndReset();
    testAdsrRejectsDerivedOverflowWithoutPublishingOutputOrState();
    return 0;
}
