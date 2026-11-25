#pragma once

#include "CheckedSpectrumPartialRange.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace talas::runtime::checked_spectrum_in_ram
{
inline constexpr std::size_t maximumLogicalFrames = 4096;
inline constexpr std::size_t maximumFramePartials =
    checked_spectrum_partial_range::maximumFramePartials;
inline constexpr std::size_t maximumTotalTriples = 1U << 20U;

struct Selection
{
    std::size_t framePartialCount = 0;
    std::size_t selectedPartialCount = 0;
    std::size_t baseFrame = 0;
    std::size_t nextFrame = 0;
    double fraction = 0.0;
};

enum class SelectionError
{
    None,
    InvalidTableShape,
    InvalidFrameCount,
    InvalidTimeIndex,
    InvalidPartialRange,
};

[[nodiscard]] inline SelectionError select(
    std::size_t totalTriples,
    std::size_t logicalFrameCount,
    double timeIndex,
    std::size_t firstPartial,
    std::size_t partialCount,
    Selection& output) noexcept
{
    output = {};
    if (logicalFrameCount == 0
        || logicalFrameCount > maximumLogicalFrames)
    {
        return SelectionError::InvalidFrameCount;
    }
    if (totalTriples == 0
        || totalTriples > maximumTotalTriples
        || totalTriples % logicalFrameCount != 0)
    {
        return SelectionError::InvalidTableShape;
    }

    const auto framePartialCount = totalTriples / logicalFrameCount;
    if (framePartialCount == 0
        || framePartialCount > maximumFramePartials)
    {
        return SelectionError::InvalidTableShape;
    }
    if (!std::isfinite(timeIndex)
        || timeIndex < -1.0 || timeIndex > 1.0)
    {
        return SelectionError::InvalidTimeIndex;
    }
    checked_spectrum_partial_range::Selection partialSelection;
    if (checked_spectrum_partial_range::select(
            framePartialCount,
            firstPartial,
            partialCount,
            partialSelection)
        != checked_spectrum_partial_range::SelectionError::None)
    {
        return SelectionError::InvalidPartialRange;
    }

    output.framePartialCount = framePartialCount;
    output.selectedPartialCount = partialSelection.selectedPartialCount;
    if (logicalFrameCount == 1U || timeIndex == -1.0)
        return SelectionError::None;
    if (timeIndex == 1.0)
    {
        output.baseFrame = logicalFrameCount - 1U;
        output.nextFrame = output.baseFrame;
        return SelectionError::None;
    }

    const auto finalFrame = static_cast<double>(logicalFrameCount - 1U);
    const auto position = std::clamp(
        (timeIndex + 1.0) * 0.5 * finalFrame,
        0.0,
        finalFrame);
    const auto baseValue = std::floor(position);
    output.baseFrame = static_cast<std::size_t>(baseValue);
    output.nextFrame = std::min(
        output.baseFrame + 1U,
        logicalFrameCount - 1U);
    output.fraction = position - baseValue;
    return SelectionError::None;
}
}
