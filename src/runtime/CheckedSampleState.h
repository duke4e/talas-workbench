#pragma once

#include "talas/core/SampleLoop.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>

namespace talas::runtime::checked_sample
{
struct State
{
    double playbackPosition = 0.0;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool gateWasPositive = false;
    bool triggerWasPositive = false;
    bool loopFadeHasWrapped = false;
    std::size_t playbackFramesElapsed = 0;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double frequency = 440.0;
    double gate = 1.0;
    double trigger = 0.0;
    double scale = 1.0;
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
    bool setLoop = false;
    double loopStart = 0.0;
    double loopEnd = 1.0;
    bool loopFade = false;
    double start = 0.0;
    double end = 1.0;
    bool gateConnected = false;
    bool triggerConnected = false;
    // The Frequency that reads one recorded frame per rendered frame.
    double unityFrequency = 440.0;
};

// A loop stored in the sample file's header,
// its first and last frames as LoopStart and LoopEnd proportions of the last
// frame, or nothing for none or a malformed one.
struct HeaderLoop
{
    double start = 0.0;
    double end = 1.0;
};

[[nodiscard]] inline std::optional<HeaderLoop> headerLoop(
    const std::optional<talas::core::SampleLoop>& loop, std::size_t frames) noexcept
{
    if (!loop.has_value() || frames < 2 || loop->startFrame >= loop->endFrameExclusive
        || loop->endFrameExclusive > frames)
    {
        return std::nullopt;
    }
    const auto lastFrame = static_cast<double>(frames - 1);
    return HeaderLoop{static_cast<double>(loop->startFrame) / lastFrame,
        static_cast<double>(loop->endFrameExclusive - 1) / lastFrame};
}

// Without SetLoop, the header's loop loops the sample while Gate is
// positive, as SetLoop's does, when it lies within the playback range.
inline void applyHeaderLoop(Config& config, const std::optional<HeaderLoop>& loop) noexcept
{
    if (config.setLoop || !loop.has_value()
        || loop->start < std::min(config.start, config.end)
        || loop->end > std::max(config.start, config.end))
    {
        return;
    }
    config.setLoop = true;
    config.loopStart = loop->start;
    config.loopEnd = loop->end;
}

enum class Error
{
    None,
    ConfigInvalid,
    TableInvalid,
    StateInvalid,
    FrequencyNonFinite,
    GateNonFinite,
    TriggerNonFinite,
    ScaleNonFinite,
    AttackInvalid,
    ReleaseInvalid,
    PlaybackStepNonFinite,
    LoopArithmeticNonFinite,
    LoopFadeArithmeticNonFinite,
    EnvelopeArithmeticNonFinite,
    InterpolationNonFinite,
    OutputNonFinite,
    PositionNonFinite,
    ElapsedFramesOverflow,
};

inline constexpr std::size_t maximumTableSamples = 1'048'576;

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double position,
    double& output) noexcept
{
    if (table.empty()
        || table.size() > maximumTableSamples
        || !std::isfinite(position))
    {
        return Error::TableInvalid;
    }
    const auto lastIndex = static_cast<double>(table.size() - 1);
    const auto clamped = std::clamp(position, 0.0, lastIndex);
    const auto leftIndex = static_cast<std::size_t>(
        std::floor(clamped));
    const auto rightIndex = std::min(
        leftIndex + 1, table.size() - 1);
    const auto amount = clamped - static_cast<double>(leftIndex);
    const auto left = table[leftIndex];
    const auto right = table[rightIndex];
    if (!std::isfinite(left)
        || !std::isfinite(right)
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

template <typename ControlsAt>
[[nodiscard]] Error render(
    const State& initial,
    const Config& config,
    std::span<const double> table,
    std::span<double> output,
    ControlsAt&& controlsAt,
    State& next) noexcept
{
    std::fill(output.begin(), output.end(), 0.0);
    const auto reject = [&] (Error error) noexcept {
        std::fill(output.begin(), output.end(), 0.0);
        return error;
    };

    const auto normalized = [] (double value) noexcept {
        return std::isfinite(value)
            && value >= 0.0
            && value <= 1.0;
    };
    const auto playbackRangeStart = std::min(
        config.start, config.end);
    const auto playbackRangeEnd = std::max(
        config.start, config.end);
    if (!std::isfinite(config.sampleRate)
        || config.sampleRate <= 0.0
        || !std::isfinite(config.unityFrequency)
        || config.unityFrequency <= 0.0
        || !normalized(config.loopStart)
        || !normalized(config.loopEnd)
        || config.loopEnd < config.loopStart
        || !normalized(config.start)
        || !normalized(config.end)
        || (config.setLoop
            && (config.loopStart < playbackRangeStart
                || config.loopEnd > playbackRangeEnd)))
    {
        return reject(Error::ConfigInvalid);
    }
    if (table.empty() || table.size() > maximumTableSamples)
        return reject(Error::TableInvalid);
    // Reverse playback ends one step before the start, so a retained
    // position may be negative.
    if (!std::isfinite(initial.playbackPosition))
        return reject(Error::StateInvalid);

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto controls = controlsAt(frame);
        if (!std::isfinite(controls.frequency))
            return reject(Error::FrequencyNonFinite);
        if (!std::isfinite(controls.gate))
            return reject(Error::GateNonFinite);
        if (!std::isfinite(controls.trigger))
            return reject(Error::TriggerNonFinite);
        if (!std::isfinite(controls.scale))
            return reject(Error::ScaleNonFinite);
        if (!std::isfinite(controls.attackSeconds)
            || controls.attackSeconds < 0.0
            || !std::isfinite(
                controls.attackSeconds * config.sampleRate))
        {
            return reject(Error::AttackInvalid);
        }
        if (!std::isfinite(controls.releaseSeconds)
            || controls.releaseSeconds < 0.0
            || !std::isfinite(
                controls.releaseSeconds * config.sampleRate))
        {
            return reject(Error::ReleaseInvalid);
        }
        const auto playbackStep =
            std::abs(controls.frequency) / config.unityFrequency;
        if (!std::isfinite(playbackStep))
            return reject(Error::PlaybackStepNonFinite);
    }

    if (output.empty())
    {
        next = initial;
        return Error::None;
    }

    const auto lastIndex = static_cast<double>(table.size() - 1);
    const auto startIndex = config.start * lastIndex;
    const auto endIndex = config.end * lastIndex;
    const auto loopStartIndex = config.loopStart * lastIndex;
    const auto loopEndIndex = config.loopEnd * lastIndex;
    const auto rangeStartIndex = std::min(startIndex, endIndex);
    const auto rangeEndIndex = std::max(startIndex, endIndex);
    const auto loopDifference = loopEndIndex - loopStartIndex;
    const auto loopSpan = loopDifference + 1.0;
    if (!std::isfinite(lastIndex)
        || !std::isfinite(startIndex)
        || !std::isfinite(endIndex)
        || !std::isfinite(loopStartIndex)
        || !std::isfinite(loopEndIndex)
        || !std::isfinite(rangeStartIndex)
        || !std::isfinite(rangeEndIndex)
        || !std::isfinite(loopDifference)
        || !std::isfinite(loopSpan)
        || loopSpan <= 0.0)
    {
        return reject(Error::LoopArithmeticNonFinite);
    }

    const auto directionForFrequency = [&] (double frequency) noexcept {
        return frequency < 0.0 || endIndex < startIndex ? -1.0 : 1.0;
    };
    const auto playbackStartForDirection = [&] (double direction) noexcept {
        return direction < 0.0 ? rangeEndIndex : startIndex;
    };
    const auto playbackEndForDirection = [&] (double direction) noexcept {
        return direction < 0.0 ? rangeStartIndex : endIndex;
    };
    const auto wrapToLoop = [&] (
        double position,
        double& wrapped) noexcept {
        const auto relative = position - loopStartIndex;
        if (!std::isfinite(relative))
            return false;
        auto remainder = std::fmod(relative, loopSpan);
        if (!std::isfinite(remainder))
            return false;
        if (remainder < 0.0)
            remainder += loopSpan;
        wrapped = loopStartIndex + remainder;
        return std::isfinite(wrapped);
    };
    const auto pastLoopBoundary = [&] (
        double position,
        double direction) noexcept {
        return direction >= 0.0
            ? position > loopEndIndex
            : position < loopStartIndex;
    };
    const auto loopFadeWindow = [&] () noexcept {
        if (loopDifference <= 0.0)
            return 0.0;
        const auto proportional = loopDifference * 0.05;
        const auto half = loopDifference * 0.5;
        const auto minimum = std::max(1.0, proportional);
        const auto result = std::min({64.0, half, minimum});
        return result;
    }();
    if (!std::isfinite(loopFadeWindow))
        return reject(Error::LoopFadeArithmeticNonFinite);

    auto candidate = initial;
    const auto firstControls = controlsAt(0);
    if (!config.gateConnected && firstControls.gate <= 0.0)
    {
        const auto direction = directionForFrequency(
            firstControls.frequency);
        candidate.playbackPosition = playbackStartForDirection(direction);
        candidate.playbackActive = false;
        candidate.playbackStarted = false;
        candidate.gateWasPositive = false;
        candidate.triggerWasPositive = false;
        candidate.loopFadeHasWrapped = false;
        candidate.playbackFramesElapsed = 0;
        next = candidate;
        return Error::None;
    }

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto controls = controlsAt(frame);
        const auto direction = directionForFrequency(controls.frequency);
        const auto playbackStart = playbackStartForDirection(direction);
        const auto playbackEnd = playbackEndForDirection(direction);
        const auto playbackStepMagnitude =
            std::abs(controls.frequency) / config.unityFrequency;
        const auto playbackStep = direction * playbackStepMagnitude;
        if (!std::isfinite(playbackStepMagnitude)
            || !std::isfinite(playbackStep))
        {
            return reject(Error::PlaybackStepNonFinite);
        }

        auto triggerRisingEdge = false;
        if (config.triggerConnected)
        {
            const auto triggerPositive = controls.trigger > 0.0;
            triggerRisingEdge = triggerPositive
                && !candidate.triggerWasPositive;
            candidate.triggerWasPositive = triggerPositive;
        }

        const auto gatePositive = controls.gate > 0.0;
        if (gatePositive)
        {
            if (triggerRisingEdge
                || !candidate.gateWasPositive
                || !candidate.playbackStarted)
            {
                candidate.playbackPosition = playbackStart;
                candidate.playbackActive = true;
                candidate.playbackStarted = true;
                candidate.loopFadeHasWrapped = false;
                candidate.playbackFramesElapsed = 0;
            }
            candidate.gateWasPositive = true;
        }
        else
        {
            candidate.gateWasPositive = false;
            if (!candidate.playbackStarted
                || !candidate.playbackActive)
            {
                candidate.playbackActive = false;
                continue;
            }
        }

        if (!candidate.playbackActive)
            continue;

        const auto loopActive = config.setLoop && gatePositive;
        const auto pastPlaybackEnd = direction >= 0.0
            ? candidate.playbackPosition > playbackEnd
            : candidate.playbackPosition < playbackEnd;
        if ((!loopActive && pastPlaybackEnd)
            || (loopActive && pastLoopBoundary(
                candidate.playbackPosition, direction)))
        {
            if (!loopActive)
            {
                candidate.playbackActive = false;
                continue;
            }
            if (!wrapToLoop(
                    candidate.playbackPosition,
                    candidate.playbackPosition))
            {
                return reject(Error::LoopArithmeticNonFinite);
            }
            candidate.loopFadeHasWrapped = true;
        }

        const auto attackFrames =
            controls.attackSeconds * config.sampleRate;
        const auto releaseFrames =
            controls.releaseSeconds * config.sampleRate;
        auto envelope = 1.0;
        if (attackFrames > 0.0)
        {
            const auto elapsed = static_cast<double>(
                candidate.playbackFramesElapsed);
            const auto attackRatio = elapsed / attackFrames;
            if (!std::isfinite(elapsed)
                || !std::isfinite(attackRatio))
            {
                return reject(Error::EnvelopeArithmeticNonFinite);
            }
            envelope *= std::min(1.0, attackRatio);
        }
        if (!config.setLoop
            && releaseFrames > 0.0
            && playbackStepMagnitude > 0.0)
        {
            const auto remainingDifference =
                playbackEnd - candidate.playbackPosition;
            const auto remainingDistance = std::abs(
                remainingDifference);
            const auto remainingRatio =
                remainingDistance / playbackStepMagnitude;
            if (!std::isfinite(remainingDifference)
                || !std::isfinite(remainingDistance))
            {
                return reject(Error::EnvelopeArithmeticNonFinite);
            }
            // A positive subnormal step can overflow the estimated remaining
            // frame count. That limit means the release is certainly still at
            // unity, so preserve the accepted hold instead of rejecting a
            // harmless saturated branch.
            if (std::isfinite(remainingRatio))
            {
                const auto remainingFrames = std::floor(remainingRatio);
                const auto releaseRatio = remainingFrames / releaseFrames;
                if (!std::isfinite(remainingFrames))
                    return reject(Error::EnvelopeArithmeticNonFinite);
                envelope *= std::isfinite(releaseRatio)
                    ? std::min(1.0, releaseRatio)
                    : 1.0;
            }
        }
        if (loopActive && config.loopFade && loopFadeWindow > 0.0)
        {
            auto loopEnvelope = 1.0;
            const auto applyFadeDistance = [&] (double distance) noexcept {
                if (!std::isfinite(distance))
                    return false;
                if (distance >= 0.0 && distance < loopFadeWindow)
                {
                    const auto ratio = distance / loopFadeWindow;
                    if (!std::isfinite(ratio))
                        return false;
                    loopEnvelope = std::min(loopEnvelope, ratio);
                }
                return true;
            };
            const auto distancesValid = direction >= 0.0
                ? applyFadeDistance(
                      loopEndIndex - candidate.playbackPosition)
                    && (!candidate.loopFadeHasWrapped
                        || applyFadeDistance(
                            candidate.playbackPosition
                            - loopStartIndex))
                : applyFadeDistance(
                      candidate.playbackPosition - loopStartIndex)
                    && (!candidate.loopFadeHasWrapped
                        || applyFadeDistance(
                            loopEndIndex
                            - candidate.playbackPosition));
            if (!distancesValid || !std::isfinite(loopEnvelope))
                return reject(Error::LoopFadeArithmeticNonFinite);
            envelope *= std::clamp(loopEnvelope, 0.0, 1.0);
        }
        if (!std::isfinite(envelope))
            return reject(Error::EnvelopeArithmeticNonFinite);

        double sampled = 0.0;
        if (const auto error = sampleLinear(
                table, candidate.playbackPosition, sampled);
            error != Error::None)
        {
            return reject(error);
        }
        const auto scaled = sampled * controls.scale;
        const auto rendered = scaled * envelope;
        if (!std::isfinite(scaled)
            || !std::isfinite(rendered))
        {
            return reject(Error::OutputNonFinite);
        }
        output[frame] = rendered;

        if (candidate.playbackFramesElapsed
            == std::numeric_limits<std::size_t>::max())
        {
            return reject(Error::ElapsedFramesOverflow);
        }
        const auto nextPosition =
            candidate.playbackPosition + playbackStep;
        if (!std::isfinite(nextPosition))
            return reject(Error::PositionNonFinite);
        candidate.playbackPosition = nextPosition;
        ++candidate.playbackFramesElapsed;
        if (loopActive && pastLoopBoundary(
                candidate.playbackPosition, direction))
        {
            if (!wrapToLoop(
                    candidate.playbackPosition,
                    candidate.playbackPosition))
            {
                return reject(Error::LoopArithmeticNonFinite);
            }
            candidate.loopFadeHasWrapped = true;
        }
        else if (!loopActive
            && (direction >= 0.0
                ? candidate.playbackPosition > playbackEnd
                : candidate.playbackPosition < playbackEnd))
        {
            candidate.playbackActive = false;
        }
    }

    next = candidate;
    return Error::None;
}
}
