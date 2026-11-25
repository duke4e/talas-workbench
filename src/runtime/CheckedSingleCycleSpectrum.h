#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <vector>

namespace talas::runtime::checked_single_cycle_spectrum
{

inline constexpr std::size_t maximumCycleSamples = 65536;
inline constexpr std::size_t maximumPartials = 4096;

struct Partial
{
    double amplitude = 0.0;
    double frequency = 0.0;
};

inline void radix2Transform(
    std::span<std::complex<double>> values, bool inverse) noexcept
{
    const auto size = values.size();
    for (std::size_t index = 1, reverse = 0; index < size; ++index)
    {
        auto bit = size >> 1U;
        while (reverse & bit)
        {
            reverse ^= bit;
            bit >>= 1U;
        }
        reverse ^= bit;
        if (index < reverse)
            std::swap(values[index], values[reverse]);
    }

    for (std::size_t length = 2; length <= size; length <<= 1U)
    {
        const auto angle = (inverse ? 2.0 : -2.0)
            * std::numbers::pi / static_cast<double>(length);
        const std::complex<double> rotation{
            std::cos(angle), std::sin(angle)};
        for (std::size_t start = 0; start < size; start += length)
        {
            std::complex<double> phase{1.0, 0.0};
            for (std::size_t offset = 0; offset < length / 2; ++offset)
            {
                const auto even = values[start + offset];
                const auto odd = values[start + offset + length / 2]
                    * phase;
                values[start + offset] = even + odd;
                values[start + offset + length / 2] = even - odd;
                phase *= rotation;
            }
        }
    }
    if (inverse)
        for (auto& value : values)
            value /= static_cast<double>(size);
}

// Bluestein's transform of a cycle of one length: the chirp and its
// transformed kernel depend only on the length, so a plan made once
// transforms any cycle of that length without allocating.
struct CoefficientPlan
{
    std::size_t count = 0;
    std::vector<std::complex<double>> chirp;
    std::vector<std::complex<double>> kernel;
    std::vector<std::complex<double>> signal;

    [[nodiscard]] std::size_t coefficientCount() const noexcept
    {
        return std::min(count / 2, maximumPartials) + 1;
    }
};

[[nodiscard]] inline std::optional<CoefficientPlan> planCoefficients(
    std::size_t count)
{
    if (count < 2 || count > maximumCycleSamples)
        return std::nullopt;
    std::size_t fftSize = 1;
    while (fftSize < 2 * count - 1)
        fftSize <<= 1U;
    CoefficientPlan plan;
    plan.count = count;
    plan.signal.resize(fftSize);
    plan.kernel.resize(fftSize);
    plan.chirp.resize(count);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto squareModulo =
            (static_cast<std::uint64_t>(index) * index)
            % (2U * count);
        const auto phase = std::numbers::pi
            * static_cast<double>(squareModulo)
            / static_cast<double>(count);
        plan.chirp[index] = {std::cos(phase), -std::sin(phase)};
        plan.kernel[index] = std::conj(plan.chirp[index]);
        if (index != 0)
            plan.kernel[fftSize - index] = plan.kernel[index];
    }
    radix2Transform(plan.kernel, false);
    return plan;
}

// The complex Fourier bins of a cycle of the plan's length into
// coefficients (coefficientCount() of them); false, leaving them unspecified,
// for a cycle of another length or one that is not finite.
[[nodiscard]] inline bool transformCoefficientsInto(
    CoefficientPlan& plan, std::span<const double> cycle,
    std::span<std::complex<double>> coefficients) noexcept
{
    const auto count = cycle.size();
    if (count != plan.count || count < 2
        || coefficients.size() != plan.coefficientCount())
        return false;
    if (!std::ranges::all_of(cycle, [](double value) {
            return std::isfinite(value);
        }))
        return false;
    std::ranges::fill(plan.signal, std::complex<double>{});
    for (std::size_t index = 0; index < count; ++index)
        plan.signal[index] = cycle[index] * plan.chirp[index];
    radix2Transform(plan.signal, false);
    for (std::size_t index = 0; index < plan.signal.size(); ++index)
        plan.signal[index] *= plan.kernel[index];
    radix2Transform(plan.signal, true);
    for (std::size_t harmonic = 0; harmonic < coefficients.size(); ++harmonic)
    {
        const auto coefficient = plan.signal[harmonic] * plan.chirp[harmonic]
            / static_cast<double>(count);
        if (!std::isfinite(coefficient.real())
            || !std::isfinite(coefficient.imag()))
            return false;
        coefficients[harmonic] = coefficient;
    }
    return true;
}

// Preserve the complex Fourier bins, including DC. Callers that synthesize
// audio need their phase; the Spectrum lane conversion below uses magnitude.
[[nodiscard]] inline std::optional<std::vector<std::complex<double>>>
transformCoefficients(std::span<const double> cycle)
{
    auto plan = planCoefficients(cycle.size());
    if (!plan.has_value())
        return std::nullopt;
    std::vector<std::complex<double>> coefficients(plan->coefficientCount());
    if (!transformCoefficientsInto(*plan, cycle, coefficients))
        return std::nullopt;
    return coefficients;
}

// Bluestein's transform keeps harmonic bins exact for non-power-of-two cycles.
// The returned partials use the native amplitude/frequency Spectrum lanes;
// phase and DC have no lane in that format.
[[nodiscard]] inline std::optional<std::vector<Partial>> transform(
    std::span<const double> cycle, double fundamentalHz)
{
    if (!std::isfinite(fundamentalHz) || fundamentalHz <= 0.0)
        return std::nullopt;
    const auto coefficients = transformCoefficients(cycle);
    if (!coefficients.has_value())
        return std::nullopt;
    const auto count = cycle.size();
    const auto partialCount = coefficients->size() - 1;
    std::vector<Partial> partials;
    partials.reserve(partialCount);
    for (std::size_t harmonic = 1; harmonic <= partialCount; ++harmonic)
    {
        const auto nyquistBin = count % 2 == 0
            && harmonic == count / 2;
        const auto amplitude = std::abs((*coefficients)[harmonic])
            * (nyquistBin ? 1.0 : 2.0);
        const auto frequency = fundamentalHz
            * static_cast<double>(harmonic);
        if (!std::isfinite(amplitude) || !std::isfinite(frequency))
            return std::nullopt;
        partials.push_back({amplitude, frequency});
    }
    return partials;
}

} // namespace talas::runtime::checked_single_cycle_spectrum
