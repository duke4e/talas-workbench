#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace talas::runtime::checked_harmonic_spectrum
{

inline constexpr std::size_t maximumPartials = 4096;

struct Partial
{
    double amplitude = 0.0;
    double frequency = 0.0;
};

[[nodiscard]] inline std::optional<std::vector<Partial>> make(
    double fundamentalHz, std::size_t count, double amplitude)
{
    if (!std::isfinite(fundamentalHz) || fundamentalHz <= 0.0
        || count == 0 || count > maximumPartials
        || !std::isfinite(amplitude) || amplitude < 0.0)
        return std::nullopt;

    std::vector<Partial> partials;
    partials.reserve(count);
    for (std::size_t harmonic = 1; harmonic <= count; ++harmonic)
    {
        // Derive each frequency from the fundamental; repeated addition
        // accumulates a growing error in the upper harmonics.
        const auto frequency = fundamentalHz * static_cast<double>(harmonic);
        if (!std::isfinite(frequency))
            return std::nullopt;
        partials.push_back({amplitude, frequency});
    }
    return partials;
}

} // namespace talas::runtime::checked_harmonic_spectrum
