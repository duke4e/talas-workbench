#pragma once

#include <cstddef>

namespace talas::runtime::checked_spectrum_partial_range
{
inline constexpr std::size_t maximumFramePartials = 4096;

struct Selection
{
    std::size_t firstIndex = 0;
    std::size_t selectedPartialCount = 0;
};

enum class SelectionError
{
    None,
    InvalidFrameWidth,
    InvalidFirstPartial,
    InvalidPartialCount,
};

[[nodiscard]] inline SelectionError select(
    std::size_t framePartialCount,
    std::size_t firstPartial,
    std::size_t requestedPartialCount,
    Selection& output) noexcept
{
    output = {};
    if (framePartialCount == 0
        || framePartialCount > maximumFramePartials)
    {
        return SelectionError::InvalidFrameWidth;
    }
    if (firstPartial == 0 || firstPartial > framePartialCount)
        return SelectionError::InvalidFirstPartial;

    const auto firstIndex = firstPartial - 1U;
    const auto remainingPartialCount = framePartialCount - firstIndex;
    const auto selectedPartialCount = requestedPartialCount == 0
        ? remainingPartialCount : requestedPartialCount;
    if (selectedPartialCount == 0
        || selectedPartialCount > maximumFramePartials
        || selectedPartialCount > remainingPartialCount)
    {
        return SelectionError::InvalidPartialCount;
    }

    output.firstIndex = firstIndex;
    output.selectedPartialCount = selectedPartialCount;
    return SelectionError::None;
}
}
