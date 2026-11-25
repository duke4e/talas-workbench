#include "talas/runtime/KernelRegistry.h"

#include "EnvelopeSegmentShape.h"
#include "FilterPoleSections.h"

#include <algorithm>
#include <cmath>

namespace talas::runtime
{
namespace
{
constexpr double twoPi = 6.283185307179586476925286766559;
constexpr double pi = 3.1415926535897932384626433832795;

[[nodiscard]] double sampleNearest(std::span<const double> samples, double phase) noexcept
{
    if (samples.size() == 1)
        return samples.front();

    auto wrappedPhase = phase - std::floor(phase);
    if (wrappedPhase >= 1.0)
        wrappedPhase = 0.0;
    const auto index = static_cast<std::size_t>(
        std::floor(wrappedPhase * static_cast<double>(samples.size()) + 0.5))
        % samples.size();
    return samples[index];
}
}

std::span<const KernelDescriptor> builtinKernelDescriptors() noexcept
{
    return talas::core::builtinKernelRegistry();
}

const KernelDescriptor* findBuiltinKernel(std::string_view typeId) noexcept
{
    return talas::core::findBuiltinKernelDescriptor(typeId);
}

KernelSignal KernelSignal::constantValue(double value) noexcept
{
    return {.constant = value};
}

KernelSignal KernelSignal::sampleValues(std::span<const double> values) noexcept
{
    return {.samples = values, .perFrame = true};
}

bool KernelSignal::validFor(std::size_t frameCount) const noexcept
{
    return !perFrame || samples.size() == frameCount;
}

double KernelSignal::valueAt(std::size_t frame) const noexcept
{
    return perFrame ? samples[frame] : constant;
}

KernelProcessStatus ClockKernel::prepare(ClockKernelConfig config) noexcept
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || !std::isfinite(config.frequency) || config.frequency <= 0.0
        || !std::isfinite(config.dutyCycle)
        || config.dutyCycle < 0.0 || config.dutyCycle > 1.0)
    {
        return KernelProcessStatus::InvalidConfiguration;
    }
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void ClockKernel::reset() noexcept
{
    phase_ = 0.0L;
    phaseEpoch_ = 0;
    nextAbsoluteFrame_ = 0;
    phaseInitialized_ = false;
}

KernelProcessStatus ClockKernel::process(
    const ClockKernelBlock& block,
    KernelProcessContext context) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    if (block.output.empty())
        return KernelProcessStatus::InvalidBlockLayout;

    if (config_.phaseContinuous)
    {
        const auto frames = block.output.size();
        if (!block.frequency.validFor(frames)
            || !block.dutyCycle.validFor(frames))
        {
            return KernelProcessStatus::InvalidBlockLayout;
        }
        if (frames > std::numeric_limits<std::uint64_t>::max()
                - context.transport.absoluteFrame)
        {
            return KernelProcessStatus::InvalidValue;
        }

        const auto wrapPhase = [](long double phase) noexcept {
            auto wrapped = std::fmod(phase, 1.0L);
            if (wrapped < 0.0L)
                wrapped += 1.0L;
            return wrapped;
        };
        const auto requiresRebase = !phaseInitialized_
            || phaseEpoch_ != context.transport.epoch
            || nextAbsoluteFrame_ != context.transport.absoluteFrame;
        auto candidatePhase = phase_;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto frequency = block.frequency.valueAt(frame);
            const auto dutyCycle = block.dutyCycle.valueAt(frame);
            if (!std::isfinite(frequency) || frequency <= 0.0
                || !std::isfinite(dutyCycle)
                || dutyCycle < 0.0 || dutyCycle > 1.0)
            {
                return KernelProcessStatus::InvalidValue;
            }
            const auto increment = static_cast<long double>(frequency)
                / static_cast<long double>(config_.sampleRate);
            if (!std::isfinite(increment))
                return KernelProcessStatus::InvalidValue;
            if (frame == 0 && requiresRebase)
            {
                candidatePhase = wrapPhase(
                    static_cast<long double>(context.transport.absoluteFrame)
                    * increment);
                if (!std::isfinite(candidatePhase))
                    return KernelProcessStatus::InvalidValue;
            }
            candidatePhase = wrapPhase(candidatePhase + increment);
            if (!std::isfinite(candidatePhase))
                return KernelProcessStatus::InvalidValue;
        }

        auto publishedPhase = phase_;
        for (std::size_t frame = 0; frame < frames; ++frame)
        {
            const auto frequency = block.frequency.valueAt(frame);
            const auto dutyCycle = block.dutyCycle.valueAt(frame);
            const auto increment = static_cast<long double>(frequency)
                / static_cast<long double>(config_.sampleRate);
            if (frame == 0 && requiresRebase)
            {
                publishedPhase = wrapPhase(
                    static_cast<long double>(context.transport.absoluteFrame)
                    * increment);
            }
            block.output[frame] = config_.enabled
                    && publishedPhase < static_cast<long double>(dutyCycle)
                ? 1.0 : 0.0;
            publishedPhase = wrapPhase(publishedPhase + increment);
        }
        phase_ = candidatePhase;
        phaseEpoch_ = context.transport.epoch;
        nextAbsoluteFrame_ = context.transport.absoluteFrame
            + static_cast<std::uint64_t>(frames);
        phaseInitialized_ = true;
        return KernelProcessStatus::Ok;
    }

    if (!config_.enabled || config_.dutyCycle == 0.0)
    {
        std::fill(block.output.begin(), block.output.end(), 0.0);
        return KernelProcessStatus::Ok;
    }
    if (config_.dutyCycle == 1.0)
    {
        std::fill(block.output.begin(), block.output.end(), 1.0);
        return KernelProcessStatus::Ok;
    }

    const auto cyclesPerFrame = static_cast<long double>(config_.frequency)
        / static_cast<long double>(config_.sampleRate);
    for (std::size_t frame = 0; frame < block.output.size(); ++frame)
    {
        const auto absoluteFrame = static_cast<long double>(context.transport.absoluteFrame)
            + static_cast<long double>(frame);
        auto phase = std::fmod(absoluteFrame * cyclesPerFrame, 1.0L);
        if (phase < 0.0L)
            phase += 1.0L;
        block.output[frame] = phase < static_cast<long double>(config_.dutyCycle)
            ? 1.0 : 0.0;
    }
    return KernelProcessStatus::Ok;
}

KernelProcessStatus ClockDividerKernel::prepare(ClockDividerKernelConfig config) noexcept
{
    if (config.division == 0 || config.steps == 0)
        return KernelProcessStatus::InvalidConfiguration;
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void ClockDividerKernel::reset() noexcept
{
    edgeCount_ = 0;
    nextStep_ = 0;
    heldStep_ = 0;
    clockWasPositive_ = false;
    gateOpen_ = false;
}

KernelProcessStatus ClockDividerKernel::process(const ClockDividerKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.stepOutput.size();
    if (frames == 0 || block.gateOutput.size() != frames
        || !block.clock.validFor(frames) || !block.reset.validFor(frames)
        || !block.division.validFor(frames) || !block.steps.validFor(frames))
    {
        return KernelProcessStatus::InvalidBlockLayout;
    }
    constexpr auto maximum = static_cast<double>(
        std::numeric_limits<std::uint32_t>::max());
    const auto validIntegerParameter = [&](double value) noexcept {
        const auto rounded = std::round(value);
        return std::isfinite(value) && value >= 1.0 && value <= maximum
            && std::abs(value - rounded) <= 1.0e-9;
    };
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        if (!std::isfinite(block.clock.valueAt(frame))
            || !std::isfinite(block.reset.valueAt(frame))
            || !validIntegerParameter(block.division.valueAt(frame))
            || !validIntegerParameter(block.steps.valueAt(frame)))
        {
            return KernelProcessStatus::InvalidValue;
        }
    }

    auto candidateConfig = config_;
    auto candidateEdgeCount = edgeCount_;
    auto candidateNextStep = nextStep_;
    auto candidateHeldStep = heldStep_;
    auto candidateClockWasPositive = clockWasPositive_;
    auto candidateGateOpen = gateOpen_;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto division = static_cast<std::uint32_t>(
            std::round(block.division.valueAt(frame)));
        const auto steps = static_cast<std::uint32_t>(
            std::round(block.steps.valueAt(frame)));
        if (division != candidateConfig.division)
        {
            candidateEdgeCount %= division;
            candidateConfig.division = division;
        }
        if (steps != candidateConfig.steps)
        {
            candidateHeldStep %= steps;
            candidateNextStep %= steps;
            candidateConfig.steps = steps;
        }
        if (block.reset.valueAt(frame) > 0.0)
        {
            candidateEdgeCount = 0;
            candidateNextStep = 0;
            candidateHeldStep = 0;
            candidateClockWasPositive = false;
            candidateGateOpen = false;
            block.stepOutput[frame] = 0.0;
            block.gateOutput[frame] = 0.0;
            continue;
        }

        const auto clockPositive = block.clock.valueAt(frame) > 0.0;
        if (clockPositive && !candidateClockWasPositive)
        {
            ++candidateEdgeCount;
            if (candidateEdgeCount >= candidateConfig.division)
            {
                candidateEdgeCount = 0;
                candidateHeldStep = candidateNextStep;
                candidateNextStep = (candidateNextStep + 1U)
                    % candidateConfig.steps;
                candidateGateOpen = true;
            }
        }
        else if (!clockPositive && candidateClockWasPositive)
        {
            candidateGateOpen = false;
        }
        candidateClockWasPositive = clockPositive;
        block.stepOutput[frame] = static_cast<double>(candidateHeldStep);
        block.gateOutput[frame] = candidateGateOpen ? 1.0 : 0.0;
    }
    config_ = candidateConfig;
    edgeCount_ = candidateEdgeCount;
    nextStep_ = candidateNextStep;
    heldStep_ = candidateHeldStep;
    clockWasPositive_ = candidateClockWasPositive;
    gateOpen_ = candidateGateOpen;
    return KernelProcessStatus::Ok;
}

KernelProcessStatus QuantizerKernel::prepare(QuantizerKernelConfig config) noexcept
{
    if (!std::isfinite(config.stepSize) || config.stepSize <= 0.0
        || !std::isfinite(config.offset))
    {
        return KernelProcessStatus::InvalidConfiguration;
    }
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void QuantizerKernel::reset() noexcept
{
}

KernelProcessStatus QuantizerKernel::process(const QuantizerKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0 || !block.value.validFor(frames)
        || !block.stepSize.validFor(frames) || !block.offset.validFor(frames))
        return KernelProcessStatus::InvalidBlockLayout;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto value = block.value.valueAt(frame);
        const auto stepSize = block.stepSize.valueAt(frame);
        const auto offset = block.offset.valueAt(frame);
        const auto quantized = offset + std::round(value) * stepSize;
        if (!std::isfinite(value) || !std::isfinite(stepSize)
            || stepSize <= 0.0 || !std::isfinite(offset)
            || !std::isfinite(quantized))
            return KernelProcessStatus::InvalidValue;
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
        block.output[frame] = block.offset.valueAt(frame)
            + std::round(block.value.valueAt(frame))
                * block.stepSize.valueAt(frame);
    return KernelProcessStatus::Ok;
}

KernelProcessStatus SchmittGateKernel::prepare(SchmittGateKernelConfig config) noexcept
{
    if (!std::isfinite(config.lowThreshold)
        || !std::isfinite(config.highThreshold)
        || config.lowThreshold >= config.highThreshold)
    {
        return KernelProcessStatus::InvalidConfiguration;
    }
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void SchmittGateKernel::reset() noexcept
{
    gateOpen_ = false;
}

KernelProcessStatus SchmittGateKernel::process(const SchmittGateKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0 || !block.value.validFor(frames)
        || !block.lowThreshold.validFor(frames)
        || !block.highThreshold.validFor(frames))
        return KernelProcessStatus::InvalidBlockLayout;
    // A HighThreshold that reaches LowThreshold stays a fixed-point step
    // above it, as a live value saturates at its range (owner, 2026-10-12,
    // taken as recommended on 2026-10-13); a static pair in that order is
    // refused as the kernel is prepared.
    const auto highThresholdAt = [&](std::size_t frame) noexcept {
        const auto lowThreshold = block.lowThreshold.valueAt(frame);
        const auto highThreshold = block.highThreshold.valueAt(frame);
        return std::isfinite(lowThreshold) && highThreshold <= lowThreshold
            ? lowThreshold + 2.0 / 16777216.0 : highThreshold;
    };
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto value = block.value.valueAt(frame);
        const auto lowThreshold = block.lowThreshold.valueAt(frame);
        const auto highThreshold = highThresholdAt(frame);
        if (!std::isfinite(value) || !std::isfinite(lowThreshold)
            || !std::isfinite(highThreshold)
            || lowThreshold >= highThreshold)
        {
            return KernelProcessStatus::InvalidValue;
        }
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto value = block.value.valueAt(frame);
        if (gateOpen_ && value <= block.lowThreshold.valueAt(frame))
            gateOpen_ = false;
        else if (!gateOpen_ && value >= highThresholdAt(frame))
            gateOpen_ = true;
        block.output[frame] = gateOpen_ ? 1.0 : 0.0;
    }
    return KernelProcessStatus::Ok;
}

KernelProcessStatus LogicKernel::prepare(LogicKernelConfig config) noexcept
{
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void LogicKernel::reset() noexcept
{
}

KernelProcessStatus LogicKernel::process(const LogicKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0 || !block.left.validFor(frames)
        || (config_.operation != LogicOperation::Not
            && !block.right.validFor(frames)))
    {
        return KernelProcessStatus::InvalidBlockLayout;
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        if (!std::isfinite(block.left.valueAt(frame))
            || (config_.operation != LogicOperation::Not
                && !std::isfinite(block.right.valueAt(frame))))
        {
            return KernelProcessStatus::InvalidValue;
        }
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto left = block.left.valueAt(frame) > 0.0;
        const auto right = config_.operation == LogicOperation::Not
            ? false : block.right.valueAt(frame) > 0.0;
        bool result = false;
        switch (config_.operation)
        {
        case LogicOperation::And:
            result = left && right;
            break;
        case LogicOperation::Or:
            result = left || right;
            break;
        case LogicOperation::Xor:
            result = left != right;
            break;
        case LogicOperation::Not:
            result = !left;
            break;
        }
        block.output[frame] = result ? 1.0 : 0.0;
    }
    return KernelProcessStatus::Ok;
}

KernelProcessStatus SlewKernel::prepare(SlewKernelConfig config) noexcept
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || !std::isfinite(config.riseTime) || config.riseTime < 0.0
        || !std::isfinite(config.fallTime) || config.fallTime < 0.0)
    {
        return KernelProcessStatus::InvalidConfiguration;
    }
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void SlewKernel::reset() noexcept
{
    currentValue_ = 0.0;
    initialized_ = false;
}

KernelProcessStatus SlewKernel::process(const SlewKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0 || !block.value.validFor(frames)
        || !block.riseTime.validFor(frames)
        || !block.fallTime.validFor(frames))
        return KernelProcessStatus::InvalidBlockLayout;

    const auto advance = [&](double currentValue,
                             double target,
                             double riseTime,
                             double fallTime) noexcept {
        const auto time = target >= currentValue ? riseTime : fallTime;
        if (time == 0.0)
            return target;
        return currentValue + (target - currentValue)
            * (1.0 - std::exp(-1.0 / (time * config_.sampleRate)));
    };
    auto candidateValue = currentValue_;
    auto candidateInitialized = initialized_;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto target = block.value.valueAt(frame);
        const auto riseTime = block.riseTime.valueAt(frame);
        const auto fallTime = block.fallTime.valueAt(frame);
        if (!std::isfinite(target) || !std::isfinite(riseTime)
            || riseTime < 0.0 || !std::isfinite(fallTime)
            || fallTime < 0.0)
            return KernelProcessStatus::InvalidValue;
        if (!candidateInitialized)
        {
            candidateValue = target;
            candidateInitialized = true;
            continue;
        }
        candidateValue = advance(
            candidateValue, target, riseTime, fallTime);
        if (!std::isfinite(candidateValue))
            return KernelProcessStatus::InvalidValue;
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto target = block.value.valueAt(frame);
        if (!initialized_)
        {
            currentValue_ = target;
            initialized_ = true;
        }
        else
        {
            currentValue_ = advance(
                currentValue_,
                target,
                block.riseTime.valueAt(frame),
                block.fallTime.valueAt(frame));
        }
        block.output[frame] = currentValue_;
    }
    return KernelProcessStatus::Ok;
}

KernelProcessStatus OscillatorKernel::prepare(OscillatorKernelConfig config) noexcept
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || !std::isfinite(config.maxModulationIndex)
        || config.maxModulationIndex < 0.0
        || (config.interpolation
                != OscillatorWavetableInterpolation::Linear
            && config.interpolation
                != OscillatorWavetableInterpolation::Nearest))
    {
        return KernelProcessStatus::InvalidConfiguration;
    }
    for (const auto sample : config.wavetable)
        if (!std::isfinite(sample))
            return KernelProcessStatus::InvalidConfiguration;

    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void OscillatorKernel::reset() noexcept
{
    phaseRadians_ = 0.0;
}

KernelProcessStatus OscillatorKernel::process(const OscillatorKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0
        || !block.frequency.validFor(frames)
        || !block.amplitude.validFor(frames)
        || !block.envelope.validFor(frames)
        || !block.pitchBend.validFor(frames)
        || !block.reset.validFor(frames)
        || !block.modulator.validFor(frames))
    {
        return KernelProcessStatus::InvalidBlockLayout;
    }

    const auto advance = [&](std::size_t frame,
                             double& phase,
                             double& output) noexcept {
        if (!std::isfinite(phase) || phase < 0.0 || phase >= twoPi)
            return KernelProcessStatus::InvalidValue;

        const auto frequency = block.frequency.valueAt(frame);
        const auto amplitude = block.amplitude.valueAt(frame);
        const auto envelope = block.envelope.valueAt(frame);
        const auto pitchBend = block.pitchBend.valueAt(frame);
        const auto resetValue = block.reset.valueAt(frame);
        // A negative frequency reads the wavetable backwards.
        if (!std::isfinite(frequency)
            || !std::isfinite(amplitude) || !std::isfinite(envelope)
            || !std::isfinite(pitchBend) || pitchBend < 0.0
            || pitchBend > 2.0 || !std::isfinite(resetValue))
        {
            return KernelProcessStatus::InvalidValue;
        }

        const auto pitchOffset = frequency * pitchBend;
        const auto pitchedFrequency = frequency + pitchOffset;
        if (!std::isfinite(pitchOffset)
            || !std::isfinite(pitchedFrequency))
        {
            return KernelProcessStatus::InvalidValue;
        }

        auto effectiveFrequency = pitchedFrequency;
        if (config_.frequencyModulation)
        {
            const auto modulator = block.modulator.valueAt(frame);
            if (!std::isfinite(modulator))
                return KernelProcessStatus::InvalidValue;
            const auto modulationAmount =
                modulator * config_.maxModulationIndex;
            const auto modulationFactor = 1.0 + modulationAmount;
            effectiveFrequency = pitchedFrequency * modulationFactor;
            const auto nyquist = config_.sampleRate * 0.5;
            if (!std::isfinite(modulationAmount)
                || !std::isfinite(modulationFactor)
                || !std::isfinite(effectiveFrequency)
                || !std::isfinite(nyquist)
                || std::abs(effectiveFrequency) >= nyquist)
            {
                return KernelProcessStatus::InvalidValue;
            }
        }

        // A nonzero Reset resets the phase.
        if (resetValue != 0.0)
            phase = 0.0;

        auto oscillator = 0.0;
        if (config_.wavetable.empty())
        {
            oscillator = std::sin(phase);
        }
        else
        {
            const auto normalizedPhase = phase / twoPi;
            if (!std::isfinite(normalizedPhase))
                return KernelProcessStatus::InvalidValue;
            if (config_.interpolation
                == OscillatorWavetableInterpolation::Nearest)
            {
                oscillator = sampleNearest(
                    config_.wavetable, normalizedPhase);
            }
            else
            {
                auto wrappedPhase =
                    normalizedPhase - std::floor(normalizedPhase);
                if (wrappedPhase >= 1.0)
                    wrappedPhase = 0.0;
                const auto scaledIndex = wrappedPhase
                    * static_cast<double>(config_.wavetable.size());
                const auto flooredIndex = std::floor(scaledIndex);
                if (!std::isfinite(wrappedPhase)
                    || !std::isfinite(scaledIndex)
                    || !std::isfinite(flooredIndex))
                {
                    return KernelProcessStatus::InvalidValue;
                }
                const auto leftIndex = static_cast<std::size_t>(
                    flooredIndex) % config_.wavetable.size();
                const auto rightIndex =
                    (leftIndex + 1) % config_.wavetable.size();
                const auto amount = scaledIndex - flooredIndex;
                const auto difference = config_.wavetable[rightIndex]
                    - config_.wavetable[leftIndex];
                const auto scaledDifference = difference * amount;
                oscillator = config_.wavetable[leftIndex]
                    + scaledDifference;
                if (!std::isfinite(amount)
                    || !std::isfinite(difference)
                    || !std::isfinite(scaledDifference))
                {
                    return KernelProcessStatus::InvalidValue;
                }
            }
        }
        if (!std::isfinite(oscillator))
            return KernelProcessStatus::InvalidValue;

        const auto scaledAmplitude = amplitude * envelope;
        const auto candidateOutput = scaledAmplitude * oscillator;
        if (!std::isfinite(scaledAmplitude)
            || !std::isfinite(candidateOutput))
        {
            return KernelProcessStatus::InvalidValue;
        }

        const auto phaseNumerator = twoPi * effectiveFrequency;
        const auto increment = phaseNumerator / config_.sampleRate;
        const auto advancedPhase = phase + increment;
        if (!std::isfinite(phaseNumerator) || !std::isfinite(increment)
            || !std::isfinite(advancedPhase))
        {
            return KernelProcessStatus::InvalidValue;
        }
        auto nextPhase = advancedPhase;
        if (nextPhase >= twoPi || nextPhase <= -twoPi)
            nextPhase = std::fmod(nextPhase, twoPi);
        if (nextPhase < 0.0)
        {
            nextPhase += twoPi;
            if (nextPhase >= twoPi)
                nextPhase = 0.0;
        }
        if (!std::isfinite(nextPhase) || nextPhase < 0.0
            || nextPhase >= twoPi)
        {
            return KernelProcessStatus::InvalidValue;
        }

        phase = nextPhase;
        output = candidateOutput;
        return KernelProcessStatus::Ok;
    };

    auto candidatePhase = phaseRadians_;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        auto candidateOutput = 0.0;
        if (advance(frame, candidatePhase, candidateOutput)
            != KernelProcessStatus::Ok)
        {
            return KernelProcessStatus::InvalidValue;
        }
    }

    auto committedPhase = phaseRadians_;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        auto committedOutput = 0.0;
        if (advance(frame, committedPhase, committedOutput)
            != KernelProcessStatus::Ok)
        {
            return KernelProcessStatus::InvalidValue;
        }
        block.output[frame] = committedOutput;
    }
    phaseRadians_ = candidatePhase;
    return KernelProcessStatus::Ok;
}

KernelProcessStatus FilterKernel::prepare(FilterKernelConfig config) noexcept
{
    // A Butterworth response is made of the Order poles; the two-pole
    // sections per Order of older projects have none.
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || config.stageCount == 0 || config.stageCount > stages_.size()
        || (config.butterworth && !config.orderPoles))
    {
        return KernelProcessStatus::InvalidConfiguration;
    }
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void FilterKernel::reset() noexcept
{
    stages_.fill({});
    feedbackSample_ = 0.0;
}

FilterKernelState FilterKernel::captureState() const noexcept
{
    FilterKernelState state;
    for (std::size_t index = 0; index < stages_.size(); ++index)
    {
        state.integratorOne[index] = stages_[index].ic1eq;
        state.integratorTwo[index] = stages_[index].ic2eq;
    }
    state.feedbackSample = feedbackSample_;
    return state;
}

KernelProcessStatus FilterKernel::restoreState(
    const FilterKernelState& state) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    if (!std::ranges::all_of(state.integratorOne, [](double value) {
            return std::isfinite(value);
        })
        || !std::ranges::all_of(state.integratorTwo, [](double value) {
            return std::isfinite(value);
        })
        || !std::isfinite(state.feedbackSample))
    {
        return KernelProcessStatus::InvalidValue;
    }
    for (std::size_t index = 0; index < stages_.size(); ++index)
    {
        stages_[index].ic1eq = state.integratorOne[index];
        stages_[index].ic2eq = state.integratorTwo[index];
    }
    feedbackSample_ = state.feedbackSample;
    return KernelProcessStatus::Ok;
}

KernelProcessStatus FilterKernel::process(const FilterKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0 || block.input.size() != frames
        || !block.frequency.validFor(frames) || !block.q.validFor(frames)
        || !block.scale.validFor(frames) || !block.feedback.validFor(frames))
    {
        return KernelProcessStatus::InvalidBlockLayout;
    }

    const auto nyquist = config_.sampleRate * 0.5;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto frequency = block.frequency.valueAt(frame);
        const auto q = block.q.valueAt(frame);
        const auto scale = block.scale.valueAt(frame);
        const auto feedback = block.feedback.valueAt(frame);
        if (!std::isfinite(block.input[frame])
            || !std::isfinite(frequency) || frequency < 0.0 || frequency >= nyquist
            || !std::isfinite(q) || q <= 0.0
            || !std::isfinite(scale) || scale < 0.0
            || !std::isfinite(feedback) || std::abs(feedback) >= 1.0)
        {
            return KernelProcessStatus::InvalidValue;
        }
        const auto g = std::tan(pi * frequency / config_.sampleRate);
        const auto damping = 1.0 / (2.0 * q);
        const auto denominator = 1.0 + 2.0 * damping * g + g * g;
        if (!std::isfinite(g) || g < 0.0
            || !std::isfinite(denominator) || denominator <= 0.0)
        {
            return KernelProcessStatus::InvalidValue;
        }
    }

    const auto processFrame = [&](double input,
                                  double frequency,
                                  double q,
                                  double scale,
                                  double feedback,
                                  auto& stages,
                                  double& feedbackSample,
                                  double& output) {
        if (config_.orderPoles)
        {
            const auto response = config_.mode == FilterKernelMode::LowPass
                ? filter_poles::Response::LowPass
                : config_.mode == FilterKernelMode::HighPass
                ? filter_poles::Response::HighPass
                : filter_poles::Response::AllPass;
            if (!filter_poles::processFrame(response, config_.stageCount,
                    config_.sampleRate, input, frequency, q, scale, feedback,
                    stages, output, config_.invertsNegativeFeedback,
                    config_.butterworth))
            {
                return false;
            }
            feedbackSample = output;
            return true;
        }
        const auto g = std::tan(pi * frequency / config_.sampleRate);
        const auto damping = 1.0 / (2.0 * q);
        const auto denominator = 1.0 + 2.0 * damping * g + g * g;
        auto sample = input + feedbackSample * feedback;
        if (!std::isfinite(sample))
            return false;
        for (std::size_t index = 0; index < config_.stageCount; ++index)
        {
            auto& stage = stages[index];
            const auto highPass =
                (sample - (2.0 * damping + g) * stage.ic1eq - stage.ic2eq) / denominator;
            const auto bandPass = g * highPass + stage.ic1eq;
            const auto lowPass = g * bandPass + stage.ic2eq;
            const auto allPass = highPass + lowPass - 2.0 * damping * bandPass;
            const auto nextIc1eq = 2.0 * bandPass - stage.ic1eq;
            const auto nextIc2eq = 2.0 * lowPass - stage.ic2eq;
            if (!std::isfinite(highPass) || !std::isfinite(bandPass)
                || !std::isfinite(lowPass) || !std::isfinite(allPass)
                || !std::isfinite(nextIc1eq)
                || !std::isfinite(nextIc2eq))
            {
                return false;
            }
            stage.ic1eq = nextIc1eq;
            stage.ic2eq = nextIc2eq;
            switch (config_.mode)
            {
            case FilterKernelMode::LowPass:
                sample = lowPass;
                break;
            case FilterKernelMode::HighPass:
                sample = highPass;
                break;
            case FilterKernelMode::AllPass:
                sample = allPass;
                break;
            }
        }
        output = sample * scale;
        if (!std::isfinite(output))
            return false;
        feedbackSample = output;
        return true;
    };

    auto candidateStages = stages_;
    auto candidateFeedbackSample = feedbackSample_;
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        double candidateOutput = 0.0;
        if (!processFrame(
                block.input[frame],
                block.frequency.valueAt(frame),
                block.q.valueAt(frame),
                block.scale.valueAt(frame),
                block.feedback.valueAt(frame),
                candidateStages,
                candidateFeedbackSample,
                candidateOutput))
        {
            return KernelProcessStatus::InvalidValue;
        }
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        if (!processFrame(
                block.input[frame],
                block.frequency.valueAt(frame),
                block.q.valueAt(frame),
                block.scale.valueAt(frame),
                block.feedback.valueAt(frame),
                stages_,
                feedbackSample_,
                block.output[frame]))
        {
            return KernelProcessStatus::InvalidValue;
        }
    }
    return KernelProcessStatus::Ok;
}

KernelProcessStatus AdsrKernel::prepare(AdsrKernelConfig config) noexcept
{
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || std::isnan(config.constantGateFalls) || config.constantGateFalls < 0.0)
        return KernelProcessStatus::InvalidConfiguration;
    config_ = config;
    prepared_ = true;
    reset();
    return KernelProcessStatus::Ok;
}

void AdsrKernel::reset() noexcept
{
    state_ = {};
}

AdsrKernelState AdsrKernel::captureState() const noexcept
{
    return state_;
}

namespace
{
[[nodiscard]] bool validAdsrState(const AdsrKernelState& state) noexcept
{
    return std::isfinite(state.elapsedTime)
        && std::isfinite(state.currentValue)
        && std::isfinite(state.releaseElapsedTime)
        && std::isfinite(state.releaseStartValue)
        && std::isfinite(state.localTime) && state.localTime >= 0.0;
}

[[nodiscard]] KernelProcessStatus applyAdsrGateEdge(
    AdsrKernelState& state,
    bool high,
    double attackTime,
    double scale,
    double legato,
    bool exponential) noexcept
{
    if (!validAdsrState(state)
        || !std::isfinite(attackTime) || attackTime <= 0.0
        || !std::isfinite(scale)
        || (exponential && scale < 0.0)
        || !std::isfinite(legato))
    {
        return KernelProcessStatus::InvalidValue;
    }
    if (high == state.gateWasPositive)
        return KernelProcessStatus::Ok;
    if (high)
    {
        auto elapsedTime = 0.0;
        if (legato > 0.0 && scale != 0.0)
        {
            const auto position = state.currentValue / scale;
            if (!std::isfinite(position))
                return KernelProcessStatus::InvalidValue;
            elapsedTime = envelope_segment::ratioFor(position, exponential) * attackTime;
            if (!std::isfinite(elapsedTime))
                return KernelProcessStatus::InvalidValue;
        }
        state.elapsedTime = elapsedTime;
        state.gateWasPositive = true;
        return KernelProcessStatus::Ok;
    }
    state.releaseElapsedTime = 0.0;
    state.releaseStartValue = state.currentValue;
    state.gateWasPositive = false;
    state.elapsedTime = 0.0;
    return KernelProcessStatus::Ok;
}

[[nodiscard]] KernelProcessStatus advanceAdsrFrame(
    AdsrKernelState& state,
    double attackTime,
    double decayTime,
    double sustainLevel,
    double releaseTime,
    double scale,
    double sampleDuration,
    bool exponential,
    double releaseSpeed,
    double& output) noexcept
{
    if (!validAdsrState(state)
        || !std::isfinite(attackTime) || attackTime <= 0.0
        || !std::isfinite(decayTime) || decayTime <= 0.0
        || !std::isfinite(sustainLevel)
        || sustainLevel < 0.0 || sustainLevel > 1.0
        || !std::isfinite(releaseTime) || releaseTime <= 0.0
        || !std::isfinite(scale)
        || (exponential && scale < 0.0)
        || !std::isfinite(sampleDuration) || sampleDuration <= 0.0)
    {
        return KernelProcessStatus::InvalidValue;
    }

    const auto decayEndTime = attackTime + decayTime;
    if (!std::isfinite(decayEndTime))
        return KernelProcessStatus::InvalidValue;

    if (state.gateWasPositive)
    {
        state.releaseElapsedTime = 0.0;
        state.releaseStartValue = state.currentValue;

        auto envelope = sustainLevel;
        if (state.elapsedTime <= attackTime)
        {
            const auto attackAmount = state.elapsedTime / attackTime;
            if (!std::isfinite(attackAmount))
                return KernelProcessStatus::InvalidValue;
            envelope = envelope_segment::covered(attackAmount, exponential);
        }
        else if (state.elapsedTime < decayEndTime)
        {
            const auto decayNumerator = state.elapsedTime - attackTime;
            const auto decayRatio = decayNumerator / decayTime;
            const auto sustainDelta = sustainLevel - 1.0;
            if (!std::isfinite(decayNumerator)
                || !std::isfinite(decayRatio)
                || !std::isfinite(sustainDelta))
            {
                return KernelProcessStatus::InvalidValue;
            }
            const auto decayAmount = envelope_segment::covered(decayRatio, exponential);
            const auto decayProduct = sustainDelta * decayAmount;
            if (!std::isfinite(decayProduct))
                return KernelProcessStatus::InvalidValue;
            envelope = 1.0 + decayProduct;
        }
        if (!std::isfinite(envelope))
            return KernelProcessStatus::InvalidValue;
        const auto currentValue = scale * envelope;
        const auto elapsedTime = state.elapsedTime + sampleDuration;
        if (!std::isfinite(currentValue) || !std::isfinite(elapsedTime))
            return KernelProcessStatus::InvalidValue;
        state.currentValue = currentValue;
        state.elapsedTime = elapsedTime > decayEndTime
            ? decayEndTime : elapsedTime;
        output = currentValue;
        return KernelProcessStatus::Ok;
    }

    const auto releaseRatio = state.releaseElapsedTime / releaseTime;
    const auto releaseRemaining = 1.0 - releaseRatio;
    if (!std::isfinite(releaseRatio)
        || !std::isfinite(releaseRemaining))
    {
        return KernelProcessStatus::InvalidValue;
    }
    const auto releaseAmount = exponential
        ? 1.0 - envelope_segment::covered(releaseRatio, true)
        : std::clamp(releaseRemaining, 0.0, 1.0);
    const auto currentValue = state.releaseStartValue * releaseAmount;
    const auto releaseElapsedTime =
        state.releaseElapsedTime + sampleDuration * releaseSpeed;
    if (!std::isfinite(currentValue)
        || !std::isfinite(releaseElapsedTime))
    {
        return KernelProcessStatus::InvalidValue;
    }
    state.elapsedTime = 0.0;
    state.currentValue = currentValue;
    state.releaseElapsedTime = releaseElapsedTime;
    output = currentValue;
    if (state.releaseElapsedTime >= releaseTime)
    {
        state.releaseElapsedTime = releaseTime;
        state.releaseStartValue = 0.0;
        state.currentValue = 0.0;
    }
    return KernelProcessStatus::Ok;
}

[[nodiscard]] KernelProcessStatus processAdsrCandidate(
    AdsrKernelState initialState,
    const AdsrKernelBlock& block,
    double sampleDuration,
    bool exponential,
    bool negativeGateDecays,
    double constantGateFalls,
    std::span<double> output,
    AdsrKernelState& candidateState) noexcept
{
    auto state = initialState;
    std::size_t typedGateEventIndex = 0;
    for (std::size_t frame = 0; frame < block.output.size(); ++frame)
    {
        const auto attackTime = block.attackTime.valueAt(frame);
        const auto decayTime = block.decayTime.valueAt(frame);
        const auto sustainLevel = block.sustainLevel.valueAt(frame);
        const auto releaseTime = block.releaseTime.valueAt(frame);
        const auto scale = block.scale.valueAt(frame);
        const auto legato = block.legato.valueAt(frame);

        if (block.useTypedGateEvents)
        {
            while (typedGateEventIndex < block.typedGateEvents.size()
                   && block.typedGateEvents[typedGateEventIndex].frameOffset
                       == frame)
            {
                const auto& event =
                    block.typedGateEvents[typedGateEventIndex++];
                if (event.targetIndex == block.typedGateTargetIndex)
                {
                    const auto status = applyAdsrGateEdge(
                        state,
                        event.high,
                        attackTime,
                        scale,
                        legato,
                        exponential);
                    if (status != KernelProcessStatus::Ok)
                        return status;
                }
            }
        }
        // A positive Gate falls at constantGateFalls.
        auto gate = block.useTypedGateEvents ? 0.0 : block.gate.valueAt(frame);
        if (gate > 0.0 && state.localTime >= constantGateFalls)
            gate = 0.0;
        if (!block.useTypedGateEvents)
        {
            const auto status = applyAdsrGateEdge(
                state,
                gate > 0.0,
                attackTime,
                scale,
                legato,
                exponential);
            if (status != KernelProcessStatus::Ok)
                return status;
        }

        auto sample = 0.0;
        const auto status = advanceAdsrFrame(
            state,
            attackTime,
            decayTime,
            sustainLevel,
            releaseTime,
            scale,
            sampleDuration,
            exponential,
            block.useTypedGateEvents
                ? 1.0
                : envelope_segment::releaseSpeed(
                    releaseTime, gate, negativeGateDecays),
            sample);
        if (status != KernelProcessStatus::Ok)
            return status;
        state.localTime += sampleDuration;
        if (!output.empty())
            output[frame] = sample;
    }
    candidateState = state;
    return KernelProcessStatus::Ok;
}
}

KernelProcessStatus AdsrKernel::restoreState(
    AdsrKernelState state) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    if (!validAdsrState(state))
        return KernelProcessStatus::InvalidValue;
    state_ = state;
    return KernelProcessStatus::Ok;
}

KernelProcessStatus AdsrKernel::applyGateEdge(
    bool high,
    double attackTime,
    double scale,
    double legato) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    auto candidate = state_;
    const auto status = applyAdsrGateEdge(
        candidate,
        high,
        attackTime,
        scale,
        legato,
        config_.exponential);
    if (status == KernelProcessStatus::Ok)
        state_ = candidate;
    return status;
}

KernelProcessStatus AdsrKernel::process(const AdsrKernelBlock& block) noexcept
{
    if (!prepared_)
        return KernelProcessStatus::NotPrepared;
    const auto frames = block.output.size();
    if (frames == 0
        || !block.attackTime.validFor(frames) || !block.decayTime.validFor(frames)
        || !block.sustainLevel.validFor(frames) || !block.releaseTime.validFor(frames)
        || !block.scale.validFor(frames)
        || (!block.useTypedGateEvents && !block.gate.validFor(frames))
        || !block.legato.validFor(frames))
    {
        return KernelProcessStatus::InvalidBlockLayout;
    }

    if (block.useTypedGateEvents)
    {
        if (block.typedGateTargetIndex == 0)
            return KernelProcessStatus::InvalidBlockLayout;
        std::size_t previousFrame = 0;
        std::uint64_t previousSequence = 0;
        bool hasPrevious = false;
        for (const auto& event : block.typedGateEvents)
        {
            if (event.targetIndex == 0
                || event.frameOffset >= frames
                || (hasPrevious
                    && (event.frameOffset < previousFrame
                        || (event.frameOffset == previousFrame
                            && event.sequence <= previousSequence))))
            {
                return KernelProcessStatus::InvalidValue;
            }
            previousFrame = event.frameOffset;
            previousSequence = event.sequence;
            hasPrevious = true;
        }
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto attackTime = block.attackTime.valueAt(frame);
        const auto decayTime = block.decayTime.valueAt(frame);
        const auto sustainLevel = block.sustainLevel.valueAt(frame);
        const auto releaseTime = block.releaseTime.valueAt(frame);
        if (!std::isfinite(attackTime) || attackTime <= 0.0
            || !std::isfinite(decayTime) || decayTime <= 0.0
            || !std::isfinite(sustainLevel) || sustainLevel < 0.0 || sustainLevel > 1.0
            || !std::isfinite(releaseTime) || releaseTime <= 0.0
            || !std::isfinite(block.scale.valueAt(frame))
            || (config_.exponential && block.scale.valueAt(frame) < 0.0)
            || (!block.useTypedGateEvents
                && !std::isfinite(block.gate.valueAt(frame)))
            || !std::isfinite(block.legato.valueAt(frame)))
        {
            return KernelProcessStatus::InvalidValue;
        }
    }

    const auto sampleDuration = 1.0 / config_.sampleRate;
    if (!std::isfinite(sampleDuration) || sampleDuration <= 0.0)
        return KernelProcessStatus::InvalidValue;

    auto candidate = AdsrKernelState{};
    const auto validationStatus = processAdsrCandidate(
        state_,
        block,
        sampleDuration,
        config_.exponential,
        config_.negativeGateDecays,
        config_.constantGateFalls,
        {},
        candidate);
    if (validationStatus != KernelProcessStatus::Ok)
        return validationStatus;

    auto publishedCandidate = AdsrKernelState{};
    const auto publicationStatus = processAdsrCandidate(
        state_,
        block,
        sampleDuration,
        config_.exponential,
        config_.negativeGateDecays,
        config_.constantGateFalls,
        block.output,
        publishedCandidate);
    if (publicationStatus != KernelProcessStatus::Ok)
        return publicationStatus;
    state_ = publishedCandidate;
    return KernelProcessStatus::Ok;
}
}
