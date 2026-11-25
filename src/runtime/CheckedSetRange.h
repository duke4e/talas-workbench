#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_set_range
{
enum class Error
{
    None,
    SizeMismatch,
    ValueNonFinite,
    OldMinNonFinite,
    OldMaxNonFinite,
    NewMinNonFinite,
    NewMaxNonFinite,
    OldSpanNonFinite,
    OldSpanZero,
    CenteredNonFinite,
    NormalizedNonFinite,
    NewSpanNonFinite,
    ScaledNonFinite,
    ResultNonFinite,
};

struct OperandBlock
{
    std::span<const double> frames;
    double fallback = 0.0;
    bool connected = false;

    [[nodiscard]] double at(std::size_t frame) const noexcept
    {
        return connected ? frames[frame] : fallback;
    }
};

// With `saturateOldSpan`, for an OldMin or OldMax that is live or fed by a
// Sound, an OldMax that reaches OldMin saturates a fixed-point step past it,
// as a live value saturates at its range (owner, 2026-10-12); a static pair
// that is equal is refused as the Sound compiles.
[[nodiscard]] inline Error advance(
    double value,
    double oldMin,
    double oldMax,
    double newMin,
    double newMax,
    double& output,
    bool saturateOldSpan = false) noexcept
{
    if (!std::isfinite(value))
        return Error::ValueNonFinite;
    if (!std::isfinite(oldMin))
        return Error::OldMinNonFinite;
    if (!std::isfinite(oldMax))
        return Error::OldMaxNonFinite;
    if (!std::isfinite(newMin))
        return Error::NewMinNonFinite;
    if (!std::isfinite(newMax))
        return Error::NewMaxNonFinite;

    auto oldSpan = oldMax - oldMin;
    if (!std::isfinite(oldSpan))
        return Error::OldSpanNonFinite;
    if (oldSpan == 0.0 && saturateOldSpan)
        oldSpan = 2.0 / 16777216.0;
    if (oldSpan == 0.0)
        return Error::OldSpanZero;

    const auto centered = value - oldMin;
    if (!std::isfinite(centered))
        return Error::CenteredNonFinite;
    const auto normalized = centered / oldSpan;
    if (!std::isfinite(normalized))
        return Error::NormalizedNonFinite;

    const auto newSpan = newMax - newMin;
    if (!std::isfinite(newSpan))
        return Error::NewSpanNonFinite;
    const auto scaled = normalized * newSpan;
    if (!std::isfinite(scaled))
        return Error::ScaledNonFinite;
    const auto result = newMin + scaled;
    if (!std::isfinite(result))
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

[[nodiscard]] inline Error advanceSelectedBlock(
    std::span<const double> values,
    OperandBlock oldMins,
    OperandBlock oldMaxes,
    OperandBlock newMins,
    OperandBlock newMaxes,
    std::span<double> output,
    bool saturateOldSpan = false) noexcept
{
    const auto invalidLaneSize = [&](const OperandBlock& lane) {
        return lane.connected && lane.frames.size() != values.size();
    };
    if (invalidLaneSize(oldMins)
        || invalidLaneSize(oldMaxes)
        || invalidLaneSize(newMins)
        || invalidLaneSize(newMaxes)
        || output.size() != values.size())
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < values.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(
            values[frame],
            oldMins.at(frame),
            oldMaxes.at(frame),
            newMins.at(frame),
            newMaxes.at(frame),
            candidate,
            saturateOldSpan);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < values.size(); ++frame)
    {
        auto candidate = 0.0;
        const auto error = advance(
            values[frame],
            oldMins.at(frame),
            oldMaxes.at(frame),
            newMins.at(frame),
            newMaxes.at(frame),
            candidate,
            saturateOldSpan);
        if (error != Error::None)
            return error;
        output[frame] = candidate;
    }
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> values,
    std::span<const double> oldMins,
    std::span<const double> oldMaxes,
    std::span<const double> newMins,
    std::span<const double> newMaxes,
    std::span<double> output) noexcept
{
    return advanceSelectedBlock(
        values,
        {oldMins, 0.0, true},
        {oldMaxes, 0.0, true},
        {newMins, 0.0, true},
        {newMaxes, 0.0, true},
        output);
}
}
