#pragma once

#include "CheckedBiquadMath.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace talas::runtime::checked_two_formant_voice
{
// An excitation signal similar to a glottal pulse, with a randomly chosen
// rate of vibrato, feeds two parallel second-order sections, the
// TwoFormantElement's. Neither the pulse's shape nor the vibrato's rates
// or depth is specified, so these are explicit candidates: Rosenberg's
// glottal pulse, opening for 40% of each period and closing for 16%, and a
// vibrato of 1% of the Frequency at a rate between 4 and 7 hz that the
// Seed chooses.
inline constexpr double openingFraction = 0.40;
inline constexpr double closingFraction = 0.16;
inline constexpr double vibratoDepth = 0.01;
inline constexpr double minimumVibratoRate = 4.0;
inline constexpr double vibratoRateSpan = 3.0;

struct State
{
    // A fraction of the period for the pulse; radians for the legacy sine.
    double pulsePhase = 0.0;
    double vibratoPhase = 0.0;
};

// Projects before format 60 excite the formants with a sine.
[[nodiscard]] inline bool singsGlottalPulse(
    const std::optional<std::string>& excitation) noexcept
{
    return !(excitation.has_value() && *excitation == "legacySine");
}

// The vibrato rate the Seed chooses, in hz.
[[nodiscard]] inline double vibratoRate(double seed) noexcept
{
    if (!std::isfinite(seed))
        return minimumVibratoRate;
    const auto bounded = std::fmod(std::floor(seed), 4294967296.0);
    auto mixed = static_cast<std::uint64_t>(static_cast<std::int64_t>(bounded))
        + 0x9e3779b97f4a7c15ULL;
    mixed = (mixed ^ (mixed >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    mixed = (mixed ^ (mixed >> 27U)) * 0x94d049bb133111ebULL;
    mixed ^= mixed >> 31U;
    const auto unit = static_cast<double>(mixed >> 11U) * 0x1.0p-53;
    return minimumVibratoRate + vibratoRateSpan * unit;
}

// One period of the pulse, for a phase from 0 to 1.
[[nodiscard]] inline double glottalPulse(double phase) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    if (phase < openingFraction)
        return 0.5 * (1.0 - std::cos(pi * phase / openingFraction));
    if (phase < openingFraction + closingFraction)
        return std::cos(0.5 * pi * (phase - openingFraction) / closingFraction);
    return 0.0;
}

// Gives this frame's excitation and advances the voice to the next frame.
// Both renderers call this one definition, so they agree to the bit.
[[gnu::noinline]] inline bool excite(
    bool glottal,
    State& state,
    double frequency,
    double rate,
    double sampleRate,
    double& excitation) noexcept
{
    if (!glottal)
    {
        const auto sine = std::sin(state.pulsePhase);
        double nextPhase = 0.0;
        if (!std::isfinite(sine)
            || !checked_biquad::advanceWrappedPhase(
                state.pulsePhase, frequency, sampleRate, nextPhase))
        {
            return false;
        }
        excitation = sine;
        state.pulsePhase = nextPhase;
        return true;
    }

    // The Frequency stays below half the sample rate, as the sine's did.
    constexpr double twoPi = 6.283185307179586476925286766559;
    if (!std::isfinite(frequency) || frequency <= 0.0
        || !std::isfinite(rate) || rate <= 0.0
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || frequency >= sampleRate * 0.5
        || !std::isfinite(state.pulsePhase) || !std::isfinite(state.vibratoPhase))
    {
        return false;
    }
    const auto pulse = glottalPulse(state.pulsePhase);
    const auto vibrato = 1.0 + vibratoDepth * std::sin(twoPi * state.vibratoPhase);
    auto nextPulse = state.pulsePhase + frequency * vibrato / sampleRate;
    auto nextVibrato = state.vibratoPhase + rate / sampleRate;
    nextPulse -= std::floor(nextPulse);
    nextVibrato -= std::floor(nextVibrato);
    if (!std::isfinite(pulse) || !std::isfinite(nextPulse)
        || !std::isfinite(nextVibrato))
    {
        return false;
    }
    excitation = pulse;
    state = {nextPulse, nextVibrato};
    return true;
}

// A formant section: the TwoFormantElement's, or for projects before
// format 60 an RBJ bandpass.
[[nodiscard]] inline bool makeSectionCoefficients(
    bool glottal,
    double sampleRate,
    double formant,
    double bandwidth,
    checked_biquad::Coefficients& coefficients) noexcept
{
    return glottal
        ? checked_biquad::makeFormantPoleCoefficients(
            sampleRate, formant, bandwidth, coefficients)
        : checked_biquad::makeBandPassCoefficients(
            sampleRate, formant, bandwidth, coefficients);
}
}
