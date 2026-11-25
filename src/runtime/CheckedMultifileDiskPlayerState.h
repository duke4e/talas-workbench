#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_multifile_disk_player
{
struct State
{
    double playbackPosition = 0.0;
    std::size_t selectedIndex = 0;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool triggerWasPositive = false;

    bool operator==(const State&) const = default;
};

struct Controls
{
    double index = 0.0;
    double trigger = 1.0;
    double rateScale = 1.0;
    bool gated = false;
};

enum class Error
{
    None,
    IndexInvalid,
    TriggerNonFinite,
    RateScaleInvalid,
    SelectionUnavailable,
    StateInvalid,
    SampleNonFinite,
    NextPositionNonFinite,
};

[[nodiscard]] inline Error resolveIndex(
    double value,
    std::size_t selectionCount,
    std::size_t& resolved) noexcept
{
    if (!std::isfinite(value) || std::trunc(value) != value)
        return Error::IndexInvalid;
    if (selectionCount == 0)
        return Error::SelectionUnavailable;
    if (value <= 0.0)
    {
        resolved = 0;
        return Error::None;
    }
    const auto last = selectionCount - 1;
    if (value >= static_cast<double>(last))
    {
        resolved = last;
        return Error::None;
    }
    resolved = static_cast<std::size_t>(value);
    return Error::None;
}

[[nodiscard]] inline Error validateControls(
    const Controls& controls,
    std::size_t selectionCount,
    std::size_t& resolvedIndex) noexcept
{
    if (const auto error = resolveIndex(
            controls.index, selectionCount, resolvedIndex);
        error != Error::None)
    {
        return error;
    }
    if (!std::isfinite(controls.trigger))
        return Error::TriggerNonFinite;
    if (!std::isfinite(controls.rateScale)
        || controls.rateScale == 0.0)
    {
        return Error::RateScaleInvalid;
    }
    return Error::None;
}

// rateAt gives, for each file, the step that plays it at its own rate,
// which a RateScale of 1 does.
template <typename TableAt, typename RateAt>
[[nodiscard]] Error advance(
    const State& state,
    const Controls& controls,
    std::size_t selectionCount,
    TableAt&& tableAt,
    RateAt&& rateAt,
    State& candidate,
    double& output)
{
    std::size_t currentSelectedIndex = 0;
    if (const auto error = validateControls(
            controls, selectionCount, currentSelectedIndex);
        error != Error::None)
    {
        return error;
    }
    if (!std::isfinite(state.playbackPosition)
        || state.selectedIndex >= selectionCount)
    {
        return Error::StateInvalid;
    }

    auto next = state;
    auto sampled = 0.0;
    const auto triggerIsPositive = controls.trigger > 0.0;
    const auto shouldTrigger =
        triggerIsPositive && !state.triggerWasPositive;
    next.triggerWasPositive = triggerIsPositive;

    if (controls.gated && !triggerIsPositive)
    {
        next.playbackActive = false;
        next.playbackStarted = false;
        candidate = next;
        output = sampled;
        return Error::None;
    }

    if (shouldTrigger)
    {
        const auto table = tableAt(currentSelectedIndex);
        if (table.empty())
            return Error::SelectionUnavailable;
        next.selectedIndex = currentSelectedIndex;
        next.playbackPosition = controls.rateScale < 0.0
            ? static_cast<double>(table.size() - 1)
            : 0.0;
        next.playbackActive = true;
        next.playbackStarted = true;
    }
    if (!shouldTrigger && !next.playbackActive)
    {
        candidate = next;
        output = sampled;
        return Error::None;
    }

    const auto table = tableAt(next.selectedIndex);
    if (table.empty())
        return Error::SelectionUnavailable;
    const auto lastIndex = static_cast<double>(table.size() - 1);
    if (!std::isfinite(lastIndex)
        || !std::isfinite(next.playbackPosition))
    {
        return Error::StateInvalid;
    }
    if (!next.playbackActive
        || next.playbackPosition < 0.0
        || next.playbackPosition > lastIndex)
    {
        next.playbackActive = false;
        candidate = next;
        output = sampled;
        return Error::None;
    }

    const auto normalizedPosition =
        next.playbackPosition / std::max(1.0, lastIndex);
    const auto scaledIndex = std::clamp(
        normalizedPosition, 0.0, 1.0) * lastIndex;
    if (!std::isfinite(normalizedPosition)
        || !std::isfinite(scaledIndex))
    {
        return Error::StateInvalid;
    }
    const auto leftIndex =
        static_cast<std::size_t>(std::floor(scaledIndex));
    const auto rightIndex = std::min(leftIndex + 1, table.size() - 1);
    const auto amount = scaledIndex - static_cast<double>(leftIndex);
    if (amount == 0.0)
    {
        sampled = table[leftIndex];
    }
    else if (amount == 1.0)
    {
        sampled = table[rightIndex];
    }
    else
    {
        const auto difference =
            table[rightIndex] - table[leftIndex];
        if (!std::isfinite(difference))
            return Error::SampleNonFinite;
        const auto offset = difference * amount;
        if (!std::isfinite(offset))
            return Error::SampleNonFinite;
        sampled = table[leftIndex] + offset;
    }
    if (!std::isfinite(sampled))
        return Error::SampleNonFinite;

    const auto nextPosition = next.playbackPosition
        + controls.rateScale * rateAt(next.selectedIndex);
    if (!std::isfinite(nextPosition))
        return Error::NextPositionNonFinite;
    next.playbackPosition = nextPosition;
    if (nextPosition < 0.0 || nextPosition > lastIndex)
        next.playbackActive = false;

    candidate = next;
    output = sampled;
    return Error::None;
}

template <typename ControlsAt, typename TableAt, typename RateAt>
[[nodiscard]] Error processBlock(
    const State& state,
    std::size_t frameCount,
    std::size_t selectionCount,
    ControlsAt&& controlsAt,
    TableAt&& tableAt,
    RateAt&& rateAt,
    State& candidate,
    std::span<double> output)
{
    if (output.size() != frameCount)
        return Error::StateInvalid;
    std::fill(output.begin(), output.end(), 0.0);

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const auto controls = controlsAt(frame);
        std::size_t resolved = 0;
        if (const auto error = validateControls(
                controls, selectionCount, resolved);
            error != Error::None)
        {
            return error;
        }
        if (tableAt(resolved).empty())
            return Error::SelectionUnavailable;
    }

    auto next = state;
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        State frameCandidate;
        double frameOutput = 0.0;
        const auto error = advance(
            next,
            controlsAt(frame),
            selectionCount,
            tableAt,
            rateAt,
            frameCandidate,
            frameOutput);
        if (error != Error::None)
        {
            std::fill(output.begin(), output.end(), 0.0);
            return error;
        }
        next = frameCandidate;
        output[frame] = frameOutput;
    }
    candidate = next;
    return Error::None;
}

// Every file plays one of its frames a frame at a RateScale of 1.
template <typename TableAt>
[[nodiscard]] Error advance(
    const State& state,
    const Controls& controls,
    std::size_t selectionCount,
    TableAt&& tableAt,
    State& candidate,
    double& output)
{
    return advance(state, controls, selectionCount, tableAt,
        [](std::size_t) { return 1.0; }, candidate, output);
}

template <typename ControlsAt, typename TableAt>
[[nodiscard]] Error processBlock(
    const State& state,
    std::size_t frameCount,
    std::size_t selectionCount,
    ControlsAt&& controlsAt,
    TableAt&& tableAt,
    State& candidate,
    std::span<double> output)
{
    return processBlock(state, frameCount, selectionCount, controlsAt, tableAt,
        [](std::size_t) { return 1.0; }, candidate, output);
}
}
