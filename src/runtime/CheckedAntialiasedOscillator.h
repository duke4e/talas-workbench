#pragma once

#include "CheckedSingleCycleSpectrum.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace talas::runtime::checked_antialiased_oscillator
{

inline constexpr std::size_t maximumPartials = 1024;

// What building a cycle's profile needs, made once for a cycle's length so
// that a DutyCycle read as each voice starts rebuilds it without allocating.
struct ProfileWorkspace
{
    std::vector<double> warped;
    checked_single_cycle_spectrum::CoefficientPlan plan;
    std::vector<std::complex<double>> coefficients;

    [[nodiscard]] std::size_t profileSize() const noexcept
    {
        return 1 + 2 * std::min(coefficients.size() - 1, maximumPartials);
    }
};

[[nodiscard]] inline std::optional<ProfileWorkspace> prepareProfileWorkspace(
    std::size_t cycleSamples)
{
    auto plan = checked_single_cycle_spectrum::planCoefficients(cycleSamples);
    if (!plan.has_value())
        return std::nullopt;
    ProfileWorkspace workspace;
    workspace.warped.resize(cycleSamples);
    workspace.coefficients.resize(plan->coefficientCount());
    workspace.plan = std::move(*plan);
    return workspace;
}

// Layout: DC, then scaled real and imaginary coefficients for harmonics 1..N.
// The even-length source Nyquist bin has one-sided gain; all other bins have
// the usual factor of two for real Fourier resynthesis. Writes the
// workspace's profileSize() values; false for a cycle of another length, one
// that is not finite, or a DutyCycle outside 0.01 to 0.99.
[[nodiscard]] inline bool buildProfileInto(
    std::span<const double> cycle, double dutyCycle,
    ProfileWorkspace& workspace, std::span<double> profile) noexcept
{
    if (cycle.size() < 2
        || cycle.size()
            > checked_single_cycle_spectrum::maximumCycleSamples
        || cycle.size() != workspace.warped.size()
        || workspace.coefficients.empty()
        || profile.size() != workspace.profileSize()
        || !std::isfinite(dutyCycle)
        || dutyCycle < 0.01 || dutyCycle > 0.99)
        return false;
    if (!std::ranges::all_of(cycle, [](double value) {
            return std::isfinite(value);
        }))
        return false;

    auto& warped = workspace.warped;
    if (dutyCycle == 0.5)
        std::ranges::copy(cycle, warped.begin());
    else
    {
        const auto count = cycle.size();
        for (std::size_t index = 0; index < count; ++index)
        {
            const auto phase = static_cast<double>(index)
                / static_cast<double>(count);
            const auto sourcePhase = phase < dutyCycle
                ? 0.5 * phase / dutyCycle
                : 0.5 + 0.5 * (phase - dutyCycle)
                    / (1.0 - dutyCycle);
            const auto sourcePosition = sourcePhase
                * static_cast<double>(count);
            const auto first = std::min(
                static_cast<std::size_t>(sourcePosition), count - 1);
            const auto next = (first + 1) % count;
            const auto fraction = sourcePosition
                - static_cast<double>(first);
            warped[index] = cycle[first]
                + (cycle[next] - cycle[first]) * fraction;
            if (!std::isfinite(warped[index]))
                return false;
        }
    }

    if (!checked_single_cycle_spectrum::transformCoefficientsInto(
            workspace.plan, warped, workspace.coefficients))
        return false;
    const auto& coefficients = workspace.coefficients;
    const auto partialCount = std::min(
        coefficients.size() - 1, maximumPartials);
    profile[0] = coefficients[0].real();
    for (std::size_t harmonic = 1; harmonic <= partialCount; ++harmonic)
    {
        const auto sourceNyquistBin = cycle.size() % 2 == 0
            && harmonic == cycle.size() / 2;
        const auto gain = sourceNyquistBin ? 1.0 : 2.0;
        profile[2 * harmonic - 1] = gain * coefficients[harmonic].real();
        profile[2 * harmonic] = gain * coefficients[harmonic].imag();
    }
    return true;
}

[[nodiscard]] inline std::optional<std::vector<double>> buildProfile(
    std::span<const double> cycle, double dutyCycle)
{
    if (!std::isfinite(dutyCycle) || dutyCycle < 0.01 || dutyCycle > 0.99)
        return std::nullopt;
    auto workspace = prepareProfileWorkspace(cycle.size());
    if (!workspace.has_value())
        return std::nullopt;
    std::vector<double> profile(workspace->profileSize());
    if (!buildProfileInto(cycle, dutyCycle, *workspace, profile))
        return std::nullopt;
    return profile;
}

[[nodiscard]] inline bool renderFrame(
    std::span<const double> profile,
    double sampleRate,
    double frequency,
    double brightness,
    double& phase,
    double& output) noexcept
{
    if (profile.empty() || profile.size() % 2 != 1
        || (profile.size() - 1) / 2 > maximumPartials
        || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(frequency)
        || !std::isfinite(brightness)
        || brightness < 0.0 || brightness > 1.0
        || !std::isfinite(phase) || phase < 0.0 || phase >= 1.0)
        return false;
    const auto increment = frequency / sampleRate;
    if (!std::isfinite(increment))
        return false;

    const auto harmonicCount = (profile.size() - 1) / 2;
    auto activeCount = harmonicCount;
    if (frequency != 0.0)
    {
        const auto ratio = (sampleRate * 0.5)
            / std::abs(frequency);
        if (ratio <= 1.0)
            activeCount = 0;
        else if (ratio <= static_cast<double>(harmonicCount))
            activeCount = static_cast<std::size_t>(
                std::ceil(ratio) - 1.0);
    }

    const auto angle = 2.0 * std::numbers::pi * phase;
    const auto baseCos = std::cos(angle);
    const auto baseSin = std::sin(angle);
    auto harmonicCos = 1.0;
    auto harmonicSin = 0.0;
    auto gain = 1.0;
    auto sample = profile[0];
    for (std::size_t harmonic = 1; harmonic <= activeCount; ++harmonic)
    {
        const auto nextCos = harmonicCos * baseCos
            - harmonicSin * baseSin;
        harmonicSin = harmonicSin * baseCos
            + harmonicCos * baseSin;
        harmonicCos = nextCos;
        const auto base = 1 + (harmonic - 1) * 2;
        sample += gain * (
            profile[base] * harmonicCos
            - profile[base + 1] * harmonicSin);
        gain *= brightness;
    }
    if (!std::isfinite(sample))
        return false;
    auto nextPhase = phase + std::remainder(increment, 1.0);
    if (nextPhase >= 1.0)
        nextPhase -= 1.0;
    else if (nextPhase < 0.0)
        nextPhase += 1.0;
    if (!std::isfinite(nextPhase) || nextPhase < 0.0
        || nextPhase >= 1.0)
        return false;
    output = sample;
    phase = nextPhase;
    return true;
}

} // namespace talas::runtime::checked_antialiased_oscillator
