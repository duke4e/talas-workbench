#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace talas::runtime::detail
{
[[nodiscard]] inline std::optional<std::pair<std::size_t, std::size_t>>
normalizeAnalogSequencerIndices(
    double start,
    double end,
    std::size_t stageCount,
    std::string_view origin) noexcept
{
    const auto legacy = origin == "legacyOneBased";
    const auto firstValue = std::round(start);
    const auto lastValue = std::round(end);
    if ((!legacy && origin != "zeroBased")
        || stageCount == 0 || stageCount > 256
        || !std::isfinite(start) || !std::isfinite(end)
        || std::abs(start - firstValue) > 1.0e-9
        || std::abs(end - lastValue) > 1.0e-9
        || start < (legacy ? 1.0 : 0.0)
        || lastValue < firstValue
        || lastValue >= static_cast<double>(stageCount) + (legacy ? 1.0 : 0.0))
        return std::nullopt;
    const auto first = static_cast<std::size_t>(firstValue) - (legacy ? 1U : 0U);
    const auto last = static_cast<std::size_t>(lastValue) - (legacy ? 1U : 0U);
    return std::pair{first, last};
}

// A StartIndex or EndIndex that follows an Event Value or a function of
// time saturates within the sequence, rounded to a stage, and a live
// EndIndex below StartIndex rises to it (a live StartIndex past a static
// EndIndex stops at it): owner decisions 2026-10-12 and 2026-10-13. A static
// index is checked as it is.
[[nodiscard]] inline std::optional<std::pair<std::size_t, std::size_t>>
selectAnalogSequencerIndices(
    double start,
    double end,
    bool startLive,
    bool endLive,
    std::size_t stageCount,
    std::string_view origin) noexcept
{
    if (stageCount == 0 || !std::isfinite(start) || !std::isfinite(end))
        return std::nullopt;
    const auto first = origin == "legacyOneBased" ? 1.0 : 0.0;
    const auto last = first + static_cast<double>(stageCount) - 1.0;
    if (startLive)
        start = std::clamp(std::round(start), first, last);
    if (endLive)
        end = std::clamp(std::round(end), first, last);
    if (std::round(end) < std::round(start))
    {
        if (endLive)
            end = start;
        else if (startLive)
            start = end;
    }
    return normalizeAnalogSequencerIndices(start, end, stageCount, origin);
}

enum class AnalogSequencerFrameError
{
    None,
    InvalidStructure,
    NonFiniteSequencePosition,
    NonFiniteOutput,
    NonFinitePositionAdvance,
};

struct AnalogSequencerFrameResult
{
    AnalogSequencerFrameError error =
        AnalogSequencerFrameError::InvalidStructure;
    double left = 0.0;
    double right = 0.0;
    double position = 0.0;
    std::optional<std::size_t> stageIndex;

    [[nodiscard]] bool ok() const noexcept
    {
        return error == AnalogSequencerFrameError::None;
    }
};

[[nodiscard]] inline AnalogSequencerFrameResult
renderAnalogSequencerFrame(
    std::span<const double> durations,
    std::span<const double> dutyCycles,
    std::span<const double> velocities,
    std::size_t startIndex,
    std::size_t endIndex,
    double selectedDuration,
    bool loop,
    double position,
    double sampleRate,
    double rate,
    std::optional<std::size_t> manualStepIndex,
    double gate,
    double input,
    double left,
    double right) noexcept
{
    if (durations.size() != dutyCycles.size()
        || durations.size() != velocities.size()
        || startIndex > endIndex || endIndex >= durations.size()
        || !std::isfinite(selectedDuration) || selectedDuration <= 0.0
        || !std::isfinite(position) || position < 0.0
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(rate) || rate == 0.0
        || !std::isfinite(gate) || !std::isfinite(input)
        || !std::isfinite(left) || !std::isfinite(right)
        || (manualStepIndex.has_value()
            && (*manualStepIndex < startIndex
                || *manualStepIndex > endIndex)))
    {
        return {};
    }

    if (gate <= 0.0)
    {
        return {
            AnalogSequencerFrameError::None,
            0.0,
            0.0,
            0.0,
        };
    }

    auto activeIndex = startIndex;
    auto active = true;
    if (manualStepIndex.has_value())
    {
        activeIndex = *manualStepIndex;
    }
    else
    {
        // The position counts frames of Rate-scaled time, so it lands on a
        // step's boundary exactly where the Duration in seconds says;
        // adding seconds a frame at a time fell short of it.
        const auto selectedFrames = selectedDuration * sampleRate;
        auto sequencePosition = position;
        if (!std::isfinite(sequencePosition))
        {
            return {
                AnalogSequencerFrameError::NonFiniteSequencePosition};
        }
        if (loop)
        {
            sequencePosition = std::fmod(
                sequencePosition, selectedFrames);
            if (!std::isfinite(sequencePosition))
            {
                return {
                    AnalogSequencerFrameError::NonFiniteSequencePosition};
            }
            if (sequencePosition < 0.0)
                sequencePosition += selectedFrames;
        }
        else if (sequencePosition >= selectedFrames)
        {
            sequencePosition = std::nextafter(selectedFrames, 0.0);
        }

        auto remaining = sequencePosition;
        for (auto offset = std::size_t{0};
             offset <= endIndex - startIndex; ++offset)
        {
            const auto index = rate > 0.0
                ? startIndex + offset : endIndex - offset;
            activeIndex = index;
            const auto stageFrames = durations[index] * sampleRate;
            if (remaining < stageFrames
                || offset == endIndex - startIndex)
                break;
            remaining -= stageFrames;
        }
        const auto activeDuration =
            durations[activeIndex] * sampleRate * dutyCycles[activeIndex];
        if (!std::isfinite(remaining)
            || !std::isfinite(activeDuration))
        {
            return {
                AnalogSequencerFrameError::NonFiniteSequencePosition};
        }
        active = remaining < activeDuration;
    }

    const auto sequencedSample = active
        ? input * velocities[activeIndex]
        : 0.0;
    const auto leftSample = sequencedSample * left;
    const auto rightSample = sequencedSample * right;
    if (!std::isfinite(sequencedSample)
        || !std::isfinite(leftSample) || !std::isfinite(rightSample))
    {
        return {AnalogSequencerFrameError::NonFiniteOutput};
    }

    // Integrate the rate at each sample so a live Rate change does not jump
    // the stage phase. Its sign selects traversal direction independently.
    const auto nextPosition = position + std::abs(rate);
    if (!std::isfinite(nextPosition))
    {
        return {AnalogSequencerFrameError::NonFinitePositionAdvance};
    }
    return {
        AnalogSequencerFrameError::None,
        leftSample,
        rightSample,
        nextPosition,
        activeIndex,
    };
}

// The Step steps through the sequence:
// each time it becomes positive, once the stage before has lasted its
// Duration, the next stage starts and sounds for its Duration times its
// DutyCycle. A Step sooner than that is ignored, so the Durations act as a
// mask, and the Gate restarts the sequence at its first stage.
struct AnalogSequencerSteps
{
    // The stage sounding, or none before the first Step after the Gate.
    std::optional<std::size_t> stage;
    // Rate-scaled frames since that stage started.
    double elapsedFrames = 0.0;
    bool stepWasPositive = false;

    bool operator==(const AnalogSequencerSteps&) const = default;
};

[[nodiscard]] inline AnalogSequencerFrameResult
renderSteppedAnalogSequencerFrame(
    std::span<const double> durations,
    std::span<const double> dutyCycles,
    std::span<const double> velocities,
    std::size_t startIndex,
    std::size_t endIndex,
    bool loop,
    double sampleRate,
    double rate,
    double stepValue,
    double gate,
    double input,
    double left,
    double right,
    AnalogSequencerSteps& steps) noexcept
{
    if (durations.size() != dutyCycles.size()
        || durations.size() != velocities.size()
        || startIndex > endIndex || endIndex >= durations.size()
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(rate) || rate == 0.0
        || !std::isfinite(stepValue)
        || !std::isfinite(gate) || !std::isfinite(input)
        || !std::isfinite(left) || !std::isfinite(right)
        || !std::isfinite(steps.elapsedFrames) || steps.elapsedFrames < 0.0
        || (steps.stage.has_value() && *steps.stage >= durations.size()))
    {
        return {};
    }

    const auto stepPositive = stepValue > 0.0;
    auto next = steps;
    next.stepWasPositive = stepPositive;
    if (gate <= 0.0)
    {
        next.stage.reset();
        next.elapsedFrames = 0.0;
        steps = next;
        return {AnalogSequencerFrameError::None, 0.0, 0.0, 0.0};
    }

    if (stepPositive && !steps.stepWasPositive
        && (!steps.stage.has_value()
            || steps.elapsedFrames >= durations[*steps.stage] * sampleRate))
    {
        // A negative Rate steps backwards; past the last stage the sequence
        // stops unless it loops. A stage left outside a range that moved
        // steps into it.
        std::optional<std::size_t> following;
        if (!steps.stage.has_value())
            following = rate > 0.0 ? startIndex : endIndex;
        else if (rate > 0.0 && *steps.stage < startIndex)
            following = startIndex;
        else if (rate > 0.0 && *steps.stage < endIndex)
            following = *steps.stage + 1;
        else if (rate < 0.0 && *steps.stage > endIndex)
            following = endIndex;
        else if (rate < 0.0 && *steps.stage > startIndex)
            following = *steps.stage - 1;
        else if (loop)
            following = rate > 0.0 ? startIndex : endIndex;
        if (following.has_value())
        {
            next.stage = following;
            next.elapsedFrames = 0.0;
        }
    }

    auto sequencedSample = 0.0;
    if (next.stage.has_value())
    {
        const auto soundingFrames = durations[*next.stage] * sampleRate
            * dutyCycles[*next.stage];
        if (!std::isfinite(soundingFrames))
            return {AnalogSequencerFrameError::NonFiniteSequencePosition};
        if (next.elapsedFrames < soundingFrames)
            sequencedSample = input * velocities[*next.stage];
    }
    const auto leftSample = sequencedSample * left;
    const auto rightSample = sequencedSample * right;
    if (!std::isfinite(sequencedSample)
        || !std::isfinite(leftSample) || !std::isfinite(rightSample))
    {
        return {AnalogSequencerFrameError::NonFiniteOutput};
    }
    if (next.stage.has_value())
    {
        next.elapsedFrames += std::abs(rate);
        if (!std::isfinite(next.elapsedFrames))
            return {AnalogSequencerFrameError::NonFinitePositionAdvance};
    }
    const auto stage = next.stage;
    steps = next;
    return {
        AnalogSequencerFrameError::None,
        leftSample,
        rightSample,
        0.0,
        stage,
    };
}
}
