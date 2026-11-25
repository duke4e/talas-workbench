#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_multisample
{
struct State
{
    double playbackPosition = 0.0;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool gateWasPositive = false;
    std::size_t selectedIndex = 0;
    std::size_t playbackFramesElapsed = 0;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double frequency = 0.0;
    double gate = 1.0;
    double index = 0.0;
    double scale = 1.0;
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};

struct Config
{
    double sampleRate = 0.0;
    double loopStart = 0.0;
    double loopEnd = 1.0;
    std::size_t tableCount = 0;
    bool loop = false;
    bool gateConnected = false;
    bool indexConnected = false;
};

enum class Error
{
    None,
    ConfigInvalid,
    FrequencyNonFinite,
    GateNonFinite,
    IndexInvalid,
    TableInvalid,
    ScaleNonFinite,
    AttackInvalid,
    ReleaseInvalid,
    PlaybackStepNonFinite,
    StateInvalid,
    LoopArithmeticNonFinite,
    EnvelopeArithmeticNonFinite,
    InterpolationNonFinite,
    OutputNonFinite,
    PositionNonFinite,
    ElapsedFramesOverflow,
};

[[nodiscard]] inline Error resolveIndex(
    double value,
    std::size_t tableCount,
    std::size_t& result) noexcept
{
    if (!std::isfinite(value)
        || std::floor(value) != value
        || value < 0.0
        || value >= static_cast<double>(tableCount))
    {
        return Error::IndexInvalid;
    }
    result = static_cast<std::size_t>(value);
    return Error::None;
}

[[nodiscard]] inline Error sampleLinear(
    std::span<const double> table,
    double position,
    double& output) noexcept
{
    if (table.empty() || !std::isfinite(position))
        return Error::TableInvalid;
    const auto lastIndex = static_cast<double>(table.size() - 1);
    const auto clamped = std::clamp(position, 0.0, lastIndex);
    const auto leftIndex = static_cast<std::size_t>(std::floor(clamped));
    const auto rightIndex = std::min(leftIndex + 1, table.size() - 1);
    const auto amount = clamped - static_cast<double>(leftIndex);
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

// unityAt gives, for each table, the Frequency that reads one of its frames
// per rendered frame, and loopAt the LoopStart and LoopEnd it loops between
// (the loop in its file's header, or else the Sound's) and the
// step that plays it at its own rate, as a Frequency of 0 hz does.
template <typename ControlsAt, typename TableAt, typename UnityAt, typename LoopAt>
[[nodiscard]] Error render(
    const State& initial,
    const Config& config,
    std::span<double> output,
    ControlsAt&& controlsAt,
    TableAt&& tableAt,
    UnityAt&& unityAt,
    LoopAt&& loopAt,
    State& next) noexcept
{
    if (!std::isfinite(config.sampleRate)
        || config.sampleRate <= 0.0
        || !std::isfinite(config.loopStart)
        || !std::isfinite(config.loopEnd)
        || config.loopStart < 0.0
        || config.loopStart > 1.0
        || config.loopEnd < config.loopStart
        || config.loopEnd > 1.0
        || config.tableCount == 0)
    {
        return Error::ConfigInvalid;
    }
    if (!std::isfinite(initial.playbackPosition)
        || initial.selectedIndex >= config.tableCount
        || tableAt(initial.selectedIndex).empty())
    {
        return Error::StateInvalid;
    }

    std::fill(output.begin(), output.end(), 0.0);
    const auto reject = [&] (Error error) noexcept {
        std::fill(output.begin(), output.end(), 0.0);
        return error;
    };

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto controls = controlsAt(frame);
        if (!std::isfinite(controls.frequency))
            return reject(Error::FrequencyNonFinite);
        if (!std::isfinite(controls.gate))
            return reject(Error::GateNonFinite);
        std::size_t resolvedIndex = 0;
        if (const auto error = resolveIndex(
                controls.index, config.tableCount, resolvedIndex);
            error != Error::None)
        {
            return reject(error);
        }
        if (tableAt(resolvedIndex).empty())
            return reject(Error::TableInvalid);
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
        const auto playbackStep = controls.frequency == 0.0
            ? loopAt(resolvedIndex)[2]
            : controls.frequency / unityAt(resolvedIndex);
        if (!std::isfinite(playbackStep))
            return reject(Error::PlaybackStepNonFinite);
    }

    if (output.empty())
    {
        next = initial;
        return Error::None;
    }

    auto candidate = initial;
    const auto firstControls = controlsAt(0);
    if (!config.gateConnected && firstControls.gate <= 0.0)
    {
        candidate.playbackActive = false;
        candidate.playbackStarted = false;
        candidate.gateWasPositive = false;
        candidate.playbackFramesElapsed = 0;
        if (!config.indexConnected)
        {
            std::size_t resolvedIndex = 0;
            if (const auto error = resolveIndex(
                    firstControls.index,
                    config.tableCount,
                    resolvedIndex);
                error != Error::None)
            {
                return reject(error);
            }
            candidate.selectedIndex = resolvedIndex;
            const auto table = tableAt(resolvedIndex);
            candidate.playbackPosition = firstControls.frequency < 0.0
                ? static_cast<double>(table.size() - 1)
                : 0.0;
        }
        next = candidate;
        return Error::None;
    }

    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        const auto controls = controlsAt(frame);
        const auto playbackReverse = controls.frequency < 0.0;
        std::size_t resolvedIndex = 0;
        if (const auto error = resolveIndex(
                controls.index, config.tableCount, resolvedIndex);
            error != Error::None)
        {
            return reject(error);
        }

        const auto gatePositive = controls.gate > 0.0;
        if (gatePositive)
        {
            if (!candidate.gateWasPositive
                || !candidate.playbackStarted)
            {
                candidate.selectedIndex = resolvedIndex;
                const auto triggerTable = tableAt(
                    candidate.selectedIndex);
                candidate.playbackPosition = playbackReverse
                    ? static_cast<double>(triggerTable.size() - 1)
                    : 0.0;
                candidate.playbackActive = true;
                candidate.playbackStarted = true;
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

        const auto table = tableAt(candidate.selectedIndex);
        if (table.empty())
            return reject(Error::TableInvalid);
        const auto [loopStart, loopEnd, originalRateStep] = loopAt(candidate.selectedIndex);
        const auto playbackStep = controls.frequency == 0.0
            ? originalRateStep
            : controls.frequency / unityAt(candidate.selectedIndex);
        const auto lastIndex = static_cast<double>(table.size() - 1);
        if (!std::isfinite(loopStart) || !std::isfinite(loopEnd)
            || loopStart < 0.0 || loopEnd > 1.0 || loopEnd < loopStart)
        {
            return reject(Error::LoopArithmeticNonFinite);
        }
        const auto loopStartIndex = loopStart * lastIndex;
        const auto loopEndIndex = loopEnd * lastIndex;
        const auto loopDifference = loopEndIndex - loopStartIndex;
        const auto loopSpan = loopDifference + 1.0;
        if (!std::isfinite(lastIndex)
            || !std::isfinite(loopStartIndex)
            || !std::isfinite(loopEndIndex)
            || !std::isfinite(loopDifference)
            || !std::isfinite(loopSpan)
            || loopSpan <= 0.0)
        {
            return reject(Error::LoopArithmeticNonFinite);
        }
        const auto playbackEnd = playbackReverse ? 0.0 : lastIndex;
        const auto pastLoopBoundary = [&] (double position) {
            return playbackReverse
                ? position < loopStartIndex
                : position > loopEndIndex;
        };
        const auto wrapToLoop = [&] (double position, double& wrapped) {
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

        const auto loopActive = config.loop && gatePositive;
        const auto pastPlaybackEnd = playbackReverse
            ? candidate.playbackPosition < playbackEnd
            : candidate.playbackPosition > playbackEnd;
        if (!candidate.playbackActive
            || (!loopActive && pastPlaybackEnd))
        {
            candidate.playbackActive = false;
            continue;
        }
        if (loopActive && pastLoopBoundary(
                candidate.playbackPosition))
        {
            if (!wrapToLoop(
                    candidate.playbackPosition,
                    candidate.playbackPosition))
            {
                return reject(Error::LoopArithmeticNonFinite);
            }
        }

        const auto attackFrames =
            controls.attackSeconds * config.sampleRate;
        const auto releaseFrames =
            controls.releaseSeconds * config.sampleRate;
        auto envelope = 1.0;
        if (attackFrames > 0.0)
        {
            const auto attackRatio = static_cast<double>(
                candidate.playbackFramesElapsed) / attackFrames;
            if (!std::isfinite(attackRatio))
                return reject(Error::EnvelopeArithmeticNonFinite);
            envelope *= std::min(1.0, attackRatio);
        }
        const auto playbackStepMagnitude = std::abs(playbackStep);
        if (!loopActive
            && releaseFrames > 0.0
            && playbackStepMagnitude > 0.0)
        {
            const auto remainingDistance = std::abs(
                playbackEnd - candidate.playbackPosition);
            const auto remainingRatio =
                remainingDistance / playbackStepMagnitude;
            if (!std::isfinite(remainingDistance)
                || !std::isfinite(remainingRatio))
            {
                return reject(Error::EnvelopeArithmeticNonFinite);
            }
            const auto remainingFrames = std::floor(remainingRatio);
            const auto releaseRatio = remainingFrames / releaseFrames;
            if (!std::isfinite(remainingFrames)
                || !std::isfinite(releaseRatio))
            {
                return reject(Error::EnvelopeArithmeticNonFinite);
            }
            envelope *= std::min(1.0, releaseRatio);
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
                candidate.playbackPosition))
        {
            if (!wrapToLoop(
                    candidate.playbackPosition,
                    candidate.playbackPosition))
            {
                return reject(Error::LoopArithmeticNonFinite);
            }
        }
        else if (playbackReverse
                ? candidate.playbackPosition < playbackEnd
                : candidate.playbackPosition > playbackEnd)
        {
            candidate.playbackActive = false;
        }
    }

    next = candidate;
    return Error::None;
}

// Every table loops between the Sound's LoopStart and LoopEnd.
template <typename ControlsAt, typename TableAt, typename UnityAt>
[[nodiscard]] Error render(
    const State& initial,
    const Config& config,
    std::span<double> output,
    ControlsAt&& controlsAt,
    TableAt&& tableAt,
    UnityAt&& unityAt,
    State& next) noexcept
{
    return render(initial, config, output, controlsAt, tableAt, unityAt,
        [&](std::size_t) { return std::array{config.loopStart, config.loopEnd, 1.0}; }, next);
}
}
