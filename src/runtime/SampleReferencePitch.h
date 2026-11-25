#pragma once

#include "talas/core/SamplePitch.h"

#include <cmath>
#include <cstdint>
#include <optional>

// A sample plays at its recorded pitch, and a sample without a single
// identifiable pitch is taken to be middle C, 4 c. A Frequency of that pitch
// plays the recording at its own rate.
namespace talas::runtime::sample_reference_pitch
{
// 4 c, MIDI note 60: 440 hz * 2^(-9/12).
inline constexpr double unpitchedFrequency = 261.6255653005986;

// The pitch a WAVE smpl chunk records, or middle C without one.
[[nodiscard]] inline double recordedFrequency(
    const std::optional<talas::core::SamplePitch>& pitch) noexcept
{
    if (!pitch.has_value() || pitch->midiUnityNote > 127U)
        return unpitchedFrequency;
    const auto note = static_cast<double>(pitch->midiUnityNote)
        + std::ldexp(static_cast<double>(pitch->midiPitchFraction), -32);
    return 440.0 * std::exp2((note - 69.0) / 12.0);
}

// The Frequency that reads one recorded frame per rendered frame: the
// recorded pitch, scaled when the recording has another sample rate.
[[nodiscard]] inline double unityFrequency(
    double recorded,
    std::uint32_t sourceSampleRate,
    double renderSampleRate) noexcept
{
    return sourceSampleRate == 0
        ? recorded
        : recorded * renderSampleRate / static_cast<double>(sourceSampleRate);
}

// The step through a recording that plays it at its own rate, at its
// recorded pitch, whatever the render's sample rate.
[[nodiscard]] inline double originalRateStep(
    std::uint32_t sourceSampleRate,
    double renderSampleRate) noexcept
{
    return sourceSampleRate == 0
        ? 1.0
        : static_cast<double>(sourceSampleRate) / renderSampleRate;
}
}
