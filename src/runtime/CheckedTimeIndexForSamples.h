#pragma once

#include "CheckedTimeIndexState.h"
#include "talas/core/SamplePitch.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace talas::runtime::checked_time_index_for_samples
{
struct FileConfig
{
    checked_time_index::Config timeIndex;
    std::size_t sourceFrames = 0;
    double baseFrequency = 0.0;
};

[[nodiscard]] inline std::optional<FileConfig> fromFileName(
    double renderSampleRate,
    std::uint32_t sourceSampleRate,
    std::size_t sourceFrames,
    std::optional<talas::core::SampleLoop> loop,
    bool looping,
    bool reverse) noexcept
{
    if (sourceFrames == 0)
        return std::nullopt;
    const auto selectedLoop = loop.value_or(
        talas::core::SampleLoop{0, sourceFrames});
    const auto config = checked_time_index::fromSampleLoop(
        renderSampleRate, sourceSampleRate, sourceFrames,
        selectedLoop, 0.0, looping, reverse);
    if (!config.has_value())
        return std::nullopt;
    return FileConfig{*config, sourceFrames, 1.0};
}

[[nodiscard]] inline std::optional<FileConfig> fromSample(
    double renderSampleRate,
    std::uint32_t sourceSampleRate,
    std::size_t sourceFrames,
    std::optional<talas::core::SampleLoop> loop,
    std::optional<talas::core::SamplePitch> pitch,
    bool looping,
    bool reverse) noexcept
{
    if (!pitch.has_value() || pitch->midiUnityNote > 127U
        || sourceFrames == 0)
        return std::nullopt;
    const auto midiNote = static_cast<double>(pitch->midiUnityNote)
        + std::ldexp(static_cast<double>(pitch->midiPitchFraction), -32);
    const auto baseFrequency =
        440.0 * std::exp2((midiNote - 69.0) / 12.0);
    if (!std::isfinite(baseFrequency) || baseFrequency <= 0.0)
        return std::nullopt;
    const auto selectedLoop = loop.value_or(
        talas::core::SampleLoop{0, sourceFrames});
    const auto config = checked_time_index::fromSampleLoop(
        renderSampleRate, sourceSampleRate, sourceFrames,
        selectedLoop, 0.0, looping, reverse);
    if (!config.has_value())
        return std::nullopt;
    return FileConfig{*config, sourceFrames, baseFrequency};
}

[[nodiscard]] inline std::optional<std::size_t> selectIndex(
    double value,
    std::size_t count) noexcept
{
    if (!std::isfinite(value) || value < 0.0
        || std::trunc(value) != value
        || value >= static_cast<double>(count))
        return std::nullopt;
    return static_cast<std::size_t>(value);
}

[[nodiscard]] inline std::optional<double> rateForFrequency(
    double frequency,
    double baseFrequency) noexcept
{
    if (!std::isfinite(frequency) || frequency < 0.0
        || !std::isfinite(baseFrequency) || baseFrequency <= 0.0)
        return std::nullopt;
    const auto rate = frequency / baseFrequency;
    return std::isfinite(rate)
        ? std::optional<double>{rate} : std::nullopt;
}
}
