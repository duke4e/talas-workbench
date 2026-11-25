#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <span>

namespace talas::runtime::checked_set_duration
{

enum class Error
{
    None,
    SampleRateInvalid,
    AudioInNonFinite,
    StartTimeInvalid,
    DurationInvalid,
    StateInvalid,
    HistoryCapacityExceeded,
    WindowInvalid,
    ElapsedOverflow,
    OutputNonFinite,
};

struct State
{
    std::size_t elapsedFrames = 0;
    std::size_t writeIndex = 0;
};

struct Transition
{
    std::size_t startFrames = 0;
    bool readsHistory = false;
    std::size_t readIndex = 0;
    bool writesHistory = false;
    std::size_t writeIndex = 0;
    double historyWrite = 0.0;
    std::size_t nextWriteIndex = 0;
    std::size_t nextElapsedFrames = 0;
    double output = 0.0;
};

[[nodiscard]] inline long double maximumIndexExclusive() noexcept
{
    return std::ldexp(
        1.0L, std::numeric_limits<std::size_t>::digits);
}

[[nodiscard]] inline Error startFramesFor(
    double startTime,
    double sampleRate,
    std::size_t& startFrames) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateInvalid;
    if (!std::isfinite(startTime) || startTime < 0.0)
        return Error::StartTimeInvalid;

    const auto scaled = static_cast<long double>(startTime)
        * static_cast<long double>(sampleRate);
    const auto rounded = std::round(scaled);
    if (!std::isfinite(rounded) || rounded < 0.0L
        || rounded >= maximumIndexExclusive())
    {
        return Error::StartTimeInvalid;
    }
    startFrames = static_cast<std::size_t>(rounded);
    return Error::None;
}

// ADR-0019: a SetDuration paced by a clock passes its Input while the clock
// has run less than Duration since it began, whatever its sample count.
[[nodiscard]] inline Error pacedOutput(
    double audioIn,
    double duration,
    double sampleRate,
    long double elapsedClockFrames,
    double& output) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateInvalid;
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;
    const auto durationFrames = static_cast<long double>(duration)
        * static_cast<long double>(sampleRate);
    if (!std::isfinite(duration) || duration <= 0.0
        || !std::isfinite(durationFrames) || durationFrames <= 0.0L)
    {
        return Error::DurationInvalid;
    }
    if (!std::isfinite(elapsedClockFrames) || elapsedClockFrames < 0.0L)
        return Error::StateInvalid;
    output = elapsedClockFrames < durationFrames ? audioIn : 0.0;
    return Error::None;
}

[[nodiscard]] inline Error advance(
    std::span<const double> history,
    State state,
    double audioIn,
    double startTime,
    double duration,
    double sampleRate,
    Transition& transition) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return Error::SampleRateInvalid;
    if (!std::isfinite(audioIn))
        return Error::AudioInNonFinite;

    std::size_t startFrames = 0;
    const auto startError = startFramesFor(
        startTime, sampleRate, startFrames);
    if (startError != Error::None)
        return startError;

    const auto durationFrames = static_cast<long double>(duration)
        * static_cast<long double>(sampleRate);
    if (!std::isfinite(duration) || duration <= 0.0
        || !std::isfinite(durationFrames) || durationFrames <= 0.0L)
    {
        return Error::DurationInvalid;
    }
    if ((history.empty() && state.writeIndex != 0)
        || (!history.empty() && state.writeIndex >= history.size()))
    {
        return Error::StateInvalid;
    }
    if (startFrames > history.size())
        return Error::HistoryCapacityExceeded;
    if (state.elapsedFrames == std::numeric_limits<std::size_t>::max())
        return Error::ElapsedOverflow;

    const auto startFrame = static_cast<long double>(startFrames);
    const auto endFrame = startFrame + durationFrames;
    if (!std::isfinite(endFrame) || endFrame <= startFrame)
        return Error::WindowInvalid;

    auto delayedInput = audioIn;
    std::size_t readIndex = 0;
    const auto readsHistory = startFrames > 0;
    if (readsHistory)
    {
        readIndex = state.writeIndex >= startFrames
            ? state.writeIndex - startFrames
            : history.size() - (startFrames - state.writeIndex);
        delayedInput = history[readIndex];
        if (!std::isfinite(delayedInput))
            return Error::StateInvalid;
    }

    const auto elapsed = static_cast<long double>(state.elapsedFrames);
    const auto output = elapsed >= startFrame && elapsed < endFrame
        ? delayedInput
        : 0.0;
    if (!std::isfinite(output))
        return Error::OutputNonFinite;

    const auto writesHistory = !history.empty();
    const auto nextWriteIndex = writesHistory
        ? (state.writeIndex + 1U) % history.size()
        : 0U;
    transition = {
        .startFrames = startFrames,
        .readsHistory = readsHistory,
        .readIndex = readIndex,
        .writesHistory = writesHistory,
        .writeIndex = state.writeIndex,
        .historyWrite = audioIn,
        .nextWriteIndex = nextWriteIndex,
        .nextElapsedFrames = state.elapsedFrames + 1U,
        .output = output,
    };
    return Error::None;
}

} // namespace talas::runtime::checked_set_duration
