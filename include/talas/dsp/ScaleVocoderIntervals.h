#pragma once

#include "talas/dsp/LinearVocoderBank.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace talas::dsp
{
inline constexpr std::size_t maximumScaleVocoderIntervalTextBytes = 2048;

enum class ScaleVocoderIntervalStatus
{
    Ok,
    TextTooLong,
    InvalidCount,
    InvalidToken,
    IntervalOutOfRange,
    IntervalsNotIncreasing,
    InvalidTonic,
    InvalidDerivedFrequency,
};

struct ScaleVocoderIntervalLayout
{
    ScaleVocoderIntervalStatus status =
        ScaleVocoderIntervalStatus::InvalidCount;
    std::array<double, maximumLinearVocoderBandCount> semitoneOffsets{};
    std::array<double, maximumLinearVocoderBandCount> centerFrequencies{};
    std::size_t count = 0;

    [[nodiscard]] std::span<const double> centers() const noexcept
    {
        return {centerFrequencies.data(), count};
    }
};

[[nodiscard]] ScaleVocoderIntervalLayout deriveScaleVocoderIntervalLayout(
    std::string_view text,
    double tonicMidiNote,
    std::size_t expectedBandCount,
    double sampleRate) noexcept;

// The layout of a scale whose intervals are already values, as braced or
// live intervals give them.
[[nodiscard]] ScaleVocoderIntervalLayout deriveScaleVocoderIntervalLayout(
    std::span<const double> intervals,
    double tonicMidiNote,
    std::size_t expectedBandCount,
    double sampleRate) noexcept;

[[nodiscard]] std::string_view scaleVocoderIntervalStatusMessage(
    ScaleVocoderIntervalStatus status) noexcept;
}
