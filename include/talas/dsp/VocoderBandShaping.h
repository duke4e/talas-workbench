#pragma once

#include "talas/dsp/LinearVocoderBank.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace talas::dsp
{
enum class VocoderBandShapingStatus
{
    Ok,
    InvalidBandCount,
    InvalidCenterFrequency,
    InvalidTone,
    InvalidRolloff,
    NumericOverflow,
};

struct VocoderBandShapingResult
{
    VocoderBandShapingStatus status =
        VocoderBandShapingStatus::InvalidBandCount;
    std::array<double, maximumLinearVocoderBandCount> carrierGains{};
    std::size_t count = 0;

    [[nodiscard]] std::span<const double> gains() const noexcept
    {
        return std::span{carrierGains}.first(count);
    }
};

// How a vocoder's Tone and Rolloff weight its carrier filters.
enum class VocoderBandShapingMode : std::uint8_t
{
    // Tone tilts the bank and Rolloff tilts it down: a ScaleVocoder's, and
    // a Vocoder's or TunableVocoder's before project format 79.
    LegacyTilt,
    // A TunableVocoder's Tone tilts the bank as far as Rolloff says, 0
    // weighting every filter alike.
    ToneWidth,
    // A Vocoder's weak tone control on its Input: edges at LoCutoff and
    // HiCutoff as steep as Rolloff says, 1 precipitous and smaller values
    // starting sooner and taking longer; a LoCutoff of 0 hz cuts nothing
    //.
    Cutoffs,
};

struct VocoderBandShaping
{
    VocoderBandShapingMode mode = VocoderBandShapingMode::LegacyTilt;
    double tone = 0.0;
    double rolloff = 0.0;
    double loCutoff = 0.0;
    double hiCutoff = 0.0;
};

[[nodiscard]] VocoderBandShapingResult deriveVocoderBandShaping(
    std::span<const double> carrierCenterFrequencies,
    const VocoderBandShaping& shaping) noexcept;

[[nodiscard]] VocoderBandShapingResult deriveVocoderBandShaping(
    std::span<const double> carrierCenterFrequencies,
    double tone,
    double rolloff) noexcept;

[[nodiscard]] LinearVocoderBankStatus configurePreparedVocoderBandShaping(
    PreparedLinearVocoderBank& bank,
    const VocoderBandShaping& shaping) noexcept;

[[nodiscard]] LinearVocoderBankStatus configurePreparedVocoderBandShaping(
    PreparedLinearVocoderBank& bank,
    double tone,
    double rolloff) noexcept;

// The tuning a Vocoder, TunableVocoder or ScaleVocoder's band fields give
// its bank: the bank's configuration, a ScaleVocoder's explicit centers and
// the band shaping's Tone and Rolloff.
struct VocoderTuning
{
    LinearVocoderBankConfiguration configuration;
    std::array<double, maximumLinearVocoderBandCount> centers{};
    bool explicitCenters = false;
    double tone = 0.0;
    double rolloff = 0.0;
    VocoderBandShapingMode shaping = VocoderBandShapingMode::LegacyTilt;
    double loCutoff = 0.0;
    double hiCutoff = 0.0;

    bool operator==(const VocoderTuning&) const = default;
};

// The hot band fields: retunes the bank, keeping its filters, envelopes
// and (for an unchanged shaping) carrier gains, and reshapes it when the
// fields give a new tuning. A tuning the bank refuses leaves the bank, and
// the last tuning, as they were.
[[nodiscard]] LinearVocoderBankStatus applyVocoderTuning(
    PreparedLinearVocoderBank& bank,
    VocoderTuning& last,
    const VocoderTuning& next) noexcept;
}
