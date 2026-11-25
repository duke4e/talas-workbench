#pragma once

#include "talas/dsp/PreparedGranularScheduler.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::runtime::checked_sample_cloud
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
    double timeIndex = 0.0;
    double durationFrames = 1.0;
    double panJitter = 0.0;
    double durationJitter = 0.0;
    double timeIndexJitter = 0.0;
};

struct Config
{
    double absoluteFrame = 0.0;
    double seed = 0.0;
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
    DurationArithmeticNonFinite,
    TimeIndexArithmeticNonFinite,
    SamplePositionNonFinite,
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
        || candidate >= uint64LimitExclusive)
    {
        return false;
    }
    if (static_cast<std::uint64_t>(candidate) != frame)
        return false;
    output = candidate;
    return true;
}

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double normalizedPosition,
    double& output) noexcept
{
    if (table.empty() || !std::isfinite(normalizedPosition))
    {
        return Error::TableInvalid;
    }
    const auto position = std::clamp(normalizedPosition, 0.0, 1.0)
        * static_cast<double>(table.size() - 1);
    if (!std::isfinite(position))
        return Error::InterpolationNonFinite;
    const auto leftIndex = static_cast<std::size_t>(std::floor(position));
    const auto rightIndex = std::min(leftIndex + 1, table.size() - 1);
    const auto amount = position - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    if (!std::isfinite(left) || !std::isfinite(right)
        || !std::isfinite(amount))
    {
        return Error::InterpolationNonFinite;
    }
    if (amount == 0.0 || leftIndex == rightIndex)
    {
        output = left;
        return Error::None;
    }
    if (amount == 1.0)
    {
        output = right;
        return Error::None;
    }
    const auto difference = right - left;
    const auto scaledDifference = difference * amount;
    const auto interpolated = left + scaledDifference;
    if (!std::isfinite(difference)
        || !std::isfinite(scaledDifference)
        || !std::isfinite(interpolated))
    {
        return Error::InterpolationNonFinite;
    }
    output = interpolated;
    return Error::None;
}

// What a grain takes from the controls of the frame it starts on: where in
// the sample it starts, its length and its pan.
struct GrainShape
{
    bool sounds = false;
    double sampleStart = 0.0;
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
    std::size_t sampleCount,
    Grain grain,
    Controls controls,
    Config config,
    GrainShape& shape) noexcept
{
    shape = {};
    const auto durationRandom =
        talas::dsp::deterministicSignedGrainRandom(
            config.seed, grain.index, 0x11ULL);
    const auto durationJitter = durationRandom * controls.durationJitter;
    const auto durationMultiplierUnclamped = 1.0 + durationJitter;
    const auto durationMultiplier = std::max(
        0.0, durationMultiplierUnclamped);
    const auto grainDurationFrames =
        controls.durationFrames * durationMultiplier;
    if (!std::isfinite(durationJitter)
        || !std::isfinite(durationMultiplierUnclamped)
        || !std::isfinite(durationMultiplier)
        || !std::isfinite(grainDurationFrames))
    {
        return Error::DurationArithmeticNonFinite;
    }
    if (grainDurationFrames <= 0.0)
        return Error::None;

    const auto timeIndexRandom =
        talas::dsp::deterministicSignedGrainRandom(
            config.seed, grain.index, 0x12ULL);
    const auto timeIndexJitter =
        timeIndexRandom * controls.timeIndexJitter;
    const auto unclampedTimeIndex = controls.timeIndex + timeIndexJitter;
    if (!std::isfinite(timeIndexJitter)
        || !std::isfinite(unclampedTimeIndex))
    {
        return Error::TimeIndexArithmeticNonFinite;
    }
    const auto timeIndex = std::clamp(unclampedTimeIndex, -1.0, 1.0);
    const auto lastSampleIndex = static_cast<double>(sampleCount - 1);
    const auto normalizedStart = (timeIndex + 1.0) * 0.5;
    const auto sampleStart = normalizedStart * lastSampleIndex;
    if (!std::isfinite(lastSampleIndex)
        || !std::isfinite(normalizedStart)
        || !std::isfinite(sampleStart))
    {
        return Error::SamplePositionNonFinite;
    }

    const auto panRandom =
        talas::dsp::deterministicSignedGrainRandom(
            config.seed, grain.index, 0x13ULL);
    const auto panJitter = panRandom * controls.panJitter;
    const auto unclampedPan = controls.pan + panJitter;
    if (!std::isfinite(panJitter) || !std::isfinite(unclampedPan))
        return Error::PanArithmeticNonFinite;
    shape = {true, sampleStart, grainDurationFrames,
        std::clamp(unclampedPan, 0.0, 1.0)};
    return Error::None;
}

// Mixes a grain into the frame. With a latch the grain keeps where it
// started, its length and pan from its first frame, its jitter added as it
// started up; without one, projects before format 74, it
// takes this frame's controls.
[[nodiscard]] inline Error mixGrain(
    std::span<const double> sampleTable,
    std::span<const double> envelopeTable,
    Grain grain,
    Controls controls,
    Config config,
    StereoFrame& mixed,
    GrainLatch* latch = nullptr) noexcept
{
    if (sampleTable.empty() || envelopeTable.empty())
    {
        return Error::TableInvalid;
    }
    if (grain.index < 0 || !std::isfinite(grain.startFrame))
        return Error::GrainInvalid;
    if (!std::isfinite(controls.amplitude)
        || !std::isfinite(controls.pan)
        || controls.pan < 0.0 || controls.pan > 1.0
        || !std::isfinite(controls.timeIndex)
        || controls.timeIndex < -1.0 || controls.timeIndex > 1.0
        || !std::isfinite(controls.durationFrames)
        || controls.durationFrames <= 0.0
        || !std::isfinite(controls.panJitter)
        || controls.panJitter < 0.0
        || !std::isfinite(controls.durationJitter)
        || controls.durationJitter < 0.0
        || !std::isfinite(controls.timeIndexJitter)
        || controls.timeIndexJitter < 0.0)
    {
        return Error::ControlsInvalid;
    }
    if (!std::isfinite(config.absoluteFrame)
        || !std::isfinite(config.seed)
        || !std::isfinite(mixed.left)
        || !std::isfinite(mixed.right))
    {
        return Error::ConfigInvalid;
    }
    if (grain.startFrame > config.absoluteFrame + 1.0e-12)
        return Error::None;

    auto shape = GrainShape{};
    if (latch != nullptr && latch->index == grain.index)
    {
        shape = latch->shape;
    }
    else
    {
        const auto shaped = shapeGrain(sampleTable.size(), grain, controls, config, shape);
        if (shaped != Error::None)
            return shaped;
        if (latch != nullptr)
            *latch = {grain.index, shape};
    }
    if (!shape.sounds)
        return Error::None;

    const auto ageFrames = config.absoluteFrame - grain.startFrame;
    if (!std::isfinite(ageFrames))
        return Error::DurationArithmeticNonFinite;
    if (ageFrames < 0.0 || ageFrames >= shape.durationFrames)
        return Error::None;

    const auto lastSampleIndex =
        static_cast<double>(sampleTable.size() - 1);
    const auto samplePosition = shape.sampleStart + ageFrames;
    if (!std::isfinite(samplePosition))
        return Error::SamplePositionNonFinite;
    if (samplePosition > lastSampleIndex)
        return Error::None;

    double sample = 0.0;
    auto error = sampleLinear(
        sampleTable,
        lastSampleIndex <= 0.0 ? 0.0 : samplePosition / lastSampleIndex,
        sample);
    if (error != Error::None)
        return error;
    double envelope = 0.0;
    error = sampleLinear(
        envelopeTable, ageFrames / shape.durationFrames, envelope);
    if (error != Error::None)
        return error;

    const auto pan = shape.pan;
    const auto leftGain = pan <= 0.5 ? 1.0 : 2.0 * (1.0 - pan);
    const auto rightGain = pan >= 0.5 ? 1.0 : 2.0 * pan;
    const auto shapedSample = sample * envelope;
    const auto grainSample = shapedSample * controls.amplitude;
    const auto leftProduct = grainSample * leftGain;
    const auto rightProduct = grainSample * rightGain;
    const auto nextLeft = mixed.left + leftProduct;
    const auto nextRight = mixed.right + rightProduct;
    if (!std::isfinite(leftGain) || !std::isfinite(rightGain)
        || !std::isfinite(shapedSample) || !std::isfinite(grainSample)
        || !std::isfinite(leftProduct) || !std::isfinite(rightProduct)
        || !std::isfinite(nextLeft) || !std::isfinite(nextRight))
    {
        return Error::OutputNonFinite;
    }

    mixed = {nextLeft, nextRight};
    return Error::None;
}
}
