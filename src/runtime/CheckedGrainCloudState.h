#pragma once

#include "talas/dsp/PreparedGranularScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::checked_grain_cloud
{
struct Grain
{
    std::int64_t index = 0;
    double startFrame = 0.0;
};

struct Controls
{
    double amplitude = 1.0;
    double pan = 0.5;
    double frequency = 0.0;
    double durationSeconds = 0.0;
    double density = 0.0;
    std::size_t maximumGrains = 1;
    double panJitter = 0.0;
    double durationJitter = 0.0;
    double frequencyJitter = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
    double cyclesPerGrain = 0.0;
    double seed = 0.0;
    double absoluteFrame = 0.0;
    // Projects before format 50 used GrainDur, or the cycles when GrainDur
    // was 0; a grain now lasts both.
    bool legacyDurationOrCycles = false;
};

struct StereoFrame
{
    double left = 0.0;
    double right = 0.0;

    bool operator==(const StereoFrame&) const = default;
};

enum class Error
{
    None,
    TableInvalid,
    GrainInvalid,
    ControlsInvalid,
    ConfigInvalid,
    CycleContractInvalid,
    FutureToleranceNonFinite,
    DurationArithmeticNonFinite,
    FrequencyArithmeticNonFinite,
    AgeArithmeticNonFinite,
    PhaseArithmeticNonFinite,
    InterpolationNonFinite,
    PanArithmeticNonFinite,
    OutputNonFinite,
};

[[nodiscard]] inline bool exactAbsoluteFrameAsDouble(
    std::uint64_t frame,
    double& output) noexcept
{
    const auto candidate = static_cast<double>(frame);
    constexpr auto uint64LimitExclusive = 0x1p64;
    if (!std::isfinite(candidate)
        || candidate >= uint64LimitExclusive
        || static_cast<std::uint64_t>(candidate) != frame)
    {
        return false;
    }
    output = candidate;
    return true;
}

[[nodiscard]] inline Error validateFrame(
    Controls controls,
    Config config) noexcept
{
    if (!std::isfinite(controls.amplitude)
        || !std::isfinite(controls.pan)
        || controls.pan < 0.0 || controls.pan > 1.0
        || !std::isfinite(controls.frequency)
        || controls.frequency < 0.0
        || !std::isfinite(controls.durationSeconds)
        || controls.durationSeconds < 0.0
        || !std::isfinite(controls.density)
        || controls.density < 0.0
        || controls.maximumGrains == 0
        || !std::isfinite(controls.panJitter)
        || controls.panJitter < 0.0
        || !std::isfinite(controls.durationJitter)
        || controls.durationJitter < 0.0
        || !std::isfinite(controls.frequencyJitter)
        || controls.frequencyJitter < 0.0)
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.sampleRate) || config.sampleRate <= 0.0
        || !std::isfinite(config.cyclesPerGrain)
        || config.cyclesPerGrain < 0.0
        || !std::isfinite(config.seed)
        || config.seed < 0.0 || config.seed > 1.0
        || !std::isfinite(config.absoluteFrame))
    {
        return Error::ConfigInvalid;
    }
    if (!config.legacyDurationOrCycles)
    {
        const auto cycleFrames = config.cyclesPerGrain * config.sampleRate;
        const auto durationFrames =
            controls.durationSeconds * config.sampleRate;
        return std::isfinite(cycleFrames) && std::isfinite(durationFrames)
            ? Error::None
            : Error::DurationArithmeticNonFinite;
    }
    if (controls.durationSeconds <= 0.0)
    {
        if (config.cyclesPerGrain <= 0.0
            || controls.frequency <= 0.0)
        {
            return Error::CycleContractInvalid;
        }
        const auto cycleFrameProduct =
            config.cyclesPerGrain * config.sampleRate;
        const auto durationFrames =
            cycleFrameProduct / controls.frequency;
        return std::isfinite(cycleFrameProduct)
                && std::isfinite(durationFrames)
                && durationFrames > 0.0
            ? Error::None
            : Error::DurationArithmeticNonFinite;
    }
    if (config.cyclesPerGrain > 0.0)
    {
        const auto cycleProduct =
            controls.frequency * controls.durationSeconds;
        const auto difference = config.cyclesPerGrain - cycleProduct;
        if (!std::isfinite(cycleProduct)
            || !std::isfinite(difference)
            || std::abs(difference) > 1.0e-9)
        {
            return Error::CycleContractInvalid;
        }
    }
    const auto durationFrames =
        controls.durationSeconds * config.sampleRate;
    return std::isfinite(durationFrames) && durationFrames > 0.0
        ? Error::None
        : Error::DurationArithmeticNonFinite;
}

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double normalizedPosition,
    double& output) noexcept
{
    if (table.empty() || !std::isfinite(normalizedPosition))
        return Error::TableInvalid;
    if (table.size() == 1)
    {
        if (!std::isfinite(table.front()))
            return Error::InterpolationNonFinite;
        output = table.front();
        return Error::None;
    }

    const auto clamped = std::clamp(normalizedPosition, 0.0, 1.0);
    const auto scaled = clamped * static_cast<double>(table.size() - 1);
    if (!std::isfinite(clamped) || !std::isfinite(scaled))
        return Error::InterpolationNonFinite;
    const auto leftIndex = static_cast<std::size_t>(std::floor(scaled));
    const auto rightIndex = std::min(leftIndex + 1, table.size() - 1);
    const auto amount = scaled - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    const auto difference = right - left;
    const auto scaledDifference = difference * amount;
    const auto interpolated = left + scaledDifference;
    if (!std::isfinite(left) || !std::isfinite(right)
        || !std::isfinite(amount) || !std::isfinite(difference)
        || !std::isfinite(scaledDifference)
        || !std::isfinite(interpolated))
    {
        return Error::InterpolationNonFinite;
    }
    output = interpolated;
    return Error::None;
}

// What a grain takes from the controls of the frame it starts on: its
// jittered frequency, length and pan.
struct GrainShape
{
    bool sounds = false;
    double frequency = 0.0;
    double durationFrames = 0.0;
    double pan = 0.5;

    bool operator==(const GrainShape&) const = default;
};

// A grain's shape kept from its first frame; index -1 holds none.
struct GrainLatch
{
    std::int64_t index = -1;
    GrainShape shape;

    bool operator==(const GrainLatch&) const = default;
};

[[nodiscard]] inline Error shapeGrain(
    Grain grain,
    Controls controls,
    Config config,
    GrainShape& shape) noexcept
{
    shape = {};
    const auto durationRandom =
        talas::dsp::deterministicSignedGrainRandom(
            config.seed, grain.index, 0x01ULL);
    const auto durationJitterProduct =
        durationRandom * controls.durationJitter;
    const auto durationMultiplierUnclamped =
        1.0 + durationJitterProduct;
    const auto durationMultiplier =
        std::max(0.0, durationMultiplierUnclamped);
    if (!std::isfinite(durationRandom)
        || !std::isfinite(durationJitterProduct)
        || !std::isfinite(durationMultiplierUnclamped)
        || !std::isfinite(durationMultiplier))
    {
        return Error::DurationArithmeticNonFinite;
    }
    if (durationMultiplier <= 0.0)
        return Error::None;

    const auto frequencyRandom =
        talas::dsp::deterministicSignedGrainRandom(
            config.seed, grain.index, 0x02ULL);
    const auto frequencyJitterProduct =
        frequencyRandom * controls.frequencyJitter;
    const auto frequencyMultiplierUnclamped =
        1.0 + frequencyJitterProduct;
    const auto frequencyMultiplier =
        std::max(0.0, frequencyMultiplierUnclamped);
    const auto grainFrequency =
        controls.frequency * frequencyMultiplier;
    if (!std::isfinite(frequencyRandom)
        || !std::isfinite(frequencyJitterProduct)
        || !std::isfinite(frequencyMultiplierUnclamped)
        || !std::isfinite(frequencyMultiplier)
        || !std::isfinite(grainFrequency))
    {
        return Error::FrequencyArithmeticNonFinite;
    }
    if (grainFrequency <= 0.0 && controls.frequencyJitter > 1.0e-12)
        return Error::None;

    double durationFrames = 0.0;
    if (!config.legacyDurationOrCycles)
    {
        // GrainDur plus CyclesPerGrain cycles of the grain's frequency; the
        // cycles of a grain at 0 hz never end, so it stays silent.
        durationFrames = controls.durationSeconds * config.sampleRate;
        if (config.cyclesPerGrain > 0.0)
        {
            if (grainFrequency <= 0.0)
                return Error::None;
            durationFrames += config.cyclesPerGrain * config.sampleRate
                / grainFrequency;
        }
        if (!std::isfinite(durationFrames))
            return Error::DurationArithmeticNonFinite;
        if (durationFrames <= 0.0)
            return Error::None;
    }
    else if (controls.durationSeconds > 0.0)
    {
        durationFrames = controls.durationSeconds * config.sampleRate;
    }
    else
    {
        if (grainFrequency <= 0.0)
            return Error::CycleContractInvalid;
        const auto cycleFrameProduct =
            config.cyclesPerGrain * config.sampleRate;
        durationFrames = cycleFrameProduct / grainFrequency;
        if (!std::isfinite(cycleFrameProduct))
            return Error::DurationArithmeticNonFinite;
    }
    const auto jitteredDurationFrames =
        durationFrames * durationMultiplier;
    if (!std::isfinite(durationFrames)
        || durationFrames <= 0.0
        || !std::isfinite(jitteredDurationFrames))
    {
        return Error::DurationArithmeticNonFinite;
    }
    if (jitteredDurationFrames <= 0.0)
        return Error::None;

    const auto panRandom =
        talas::dsp::deterministicSignedGrainRandom(
            config.seed, grain.index, 0x03ULL);
    const auto panJitterProduct = panRandom * controls.panJitter;
    const auto unclampedPan = controls.pan + panJitterProduct;
    if (!std::isfinite(panRandom)
        || !std::isfinite(panJitterProduct)
        || !std::isfinite(unclampedPan))
    {
        return Error::PanArithmeticNonFinite;
    }
    shape = {true, grainFrequency, jitteredDurationFrames,
        std::clamp(unclampedPan, 0.0, 1.0)};
    return Error::None;
}

// Mixes a grain into the frame. With a latch the grain keeps the shape it
// took on its first frame, its jitter added as it started up;
// without one, projects before format 74, it takes this frame's controls.
[[nodiscard]] inline Error mixGrain(
    std::span<const double> waveform,
    std::span<const double> envelope,
    Grain grain,
    Controls controls,
    Config config,
    StereoFrame& mixed,
    GrainLatch* latch = nullptr) noexcept
{
    if (waveform.empty() || envelope.empty())
        return Error::TableInvalid;
    if (grain.index < 0 || !std::isfinite(grain.startFrame))
        return Error::GrainInvalid;
    const auto validation = validateFrame(controls, config);
    if (validation != Error::None)
        return validation;
    if (!std::isfinite(mixed.left) || !std::isfinite(mixed.right))
        return Error::OutputNonFinite;

    const auto inclusiveFrame = config.absoluteFrame + 1.0e-12;
    if (!std::isfinite(inclusiveFrame))
        return Error::FutureToleranceNonFinite;
    if (grain.startFrame > inclusiveFrame)
        return Error::None;

    auto shape = GrainShape{};
    if (latch != nullptr && latch->index == grain.index)
    {
        shape = latch->shape;
    }
    else
    {
        const auto shaped = shapeGrain(grain, controls, config, shape);
        if (shaped != Error::None)
            return shaped;
        if (latch != nullptr)
            *latch = {grain.index, shape};
    }
    if (!shape.sounds)
        return Error::None;

    const auto ageFrames = config.absoluteFrame - grain.startFrame;
    if (!std::isfinite(ageFrames))
        return Error::AgeArithmeticNonFinite;
    if (ageFrames < 0.0 || ageFrames >= shape.durationFrames)
        return Error::None;

    const auto ageSeconds = ageFrames / config.sampleRate;
    const auto phaseProduct = ageSeconds * shape.frequency;
    auto phase = std::fmod(phaseProduct, 1.0);
    if (!std::isfinite(ageSeconds)
        || !std::isfinite(phaseProduct)
        || !std::isfinite(phase))
    {
        return Error::PhaseArithmeticNonFinite;
    }
    if (phase < 0.0)
        phase += 1.0;
    if (!std::isfinite(phase))
        return Error::PhaseArithmeticNonFinite;

    double waveformSample = 0.0;
    auto error = sampleLinear(waveform, phase, waveformSample);
    if (error != Error::None)
        return error;
    double envelopeSample = 0.0;
    error = sampleLinear(
        envelope, ageFrames / shape.durationFrames, envelopeSample);
    if (error != Error::None)
        return error;

    const auto pan = shape.pan;
    const auto leftGain = pan <= 0.5 ? 1.0 : 2.0 * (1.0 - pan);
    const auto rightGain = pan >= 0.5 ? 1.0 : 2.0 * pan;
    const auto shaped = waveformSample * envelopeSample;
    const auto sample = shaped * controls.amplitude;
    const auto leftProduct = sample * leftGain;
    const auto rightProduct = sample * rightGain;
    const auto nextLeft = mixed.left + leftProduct;
    const auto nextRight = mixed.right + rightProduct;
    if (!std::isfinite(pan) || !std::isfinite(leftGain)
        || !std::isfinite(rightGain) || !std::isfinite(shaped)
        || !std::isfinite(sample) || !std::isfinite(leftProduct)
        || !std::isfinite(rightProduct) || !std::isfinite(nextLeft)
        || !std::isfinite(nextRight))
    {
        return Error::OutputNonFinite;
    }

    mixed = {nextLeft, nextRight};
    return Error::None;
}
}
