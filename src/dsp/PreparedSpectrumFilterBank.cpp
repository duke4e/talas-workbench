#include "talas/dsp/PreparedSpectrumFilterBank.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace talas::dsp
{
namespace
{
struct Coefficients
{
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

[[nodiscard]] Coefficients makeBandPassCoefficients(
    double sampleRate,
    double centerFrequency,
    double bandwidth) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    const auto q = centerFrequency / bandwidth;
    const auto omega = 2.0 * pi * centerFrequency / sampleRate;
    const auto sinOmega = std::sin(omega);
    const auto cosOmega = std::cos(omega);
    const auto alpha = sinOmega / (2.0 * q);
    const auto a0 = 1.0 + alpha;
    return {
        alpha / a0,
        0.0,
        -alpha / a0,
        -2.0 * cosOmega / a0,
        (1.0 - alpha) / a0,
    };
}

[[nodiscard]] bool finite(const Coefficients& value) noexcept
{
    return std::isfinite(value.b0) && std::isfinite(value.b1)
        && std::isfinite(value.b2) && std::isfinite(value.a1)
        && std::isfinite(value.a2);
}

template <typename State>
[[nodiscard]] double processSample(
    const Coefficients& coefficients,
    State& state,
    double input) noexcept
{
    const auto output = coefficients.b0 * input
        + coefficients.b1 * state.x1
        + coefficients.b2 * state.x2
        - coefficients.a1 * state.y1
        - coefficients.a2 * state.y2;
    state.x2 = state.x1;
    state.x1 = input;
    state.y2 = state.y1;
    state.y1 = output;
    return output;
}
}

double spectrumFilterSpacing(
    double below,
    double center,
    double above) noexcept
{
    auto spacing = std::numeric_limits<double>::infinity();
    for (const auto neighbour : {below, above})
    {
        const auto distance = std::abs(center - neighbour);
        if (std::isfinite(distance) && distance > 0.0)
            spacing = std::min(spacing, distance);
    }
    return std::isfinite(spacing) ? spacing : std::abs(center);
}

double spectrumFilterBandwidth(
    SpectrumFilterBandwidthUnits units,
    double control,
    double width) noexcept
{
    if (units == SpectrumFilterBandwidthUnits::Spacing)
        return control * width;
    return control > 0.0 ? control : width;
}

PreparedSpectrumFilterBankStatus PreparedSpectrumFilterBank::configure(
    double sampleRate,
    std::span<const SpectrumFilterBandConfiguration> bands,
    SpectrumFilterBandwidthUnits units) noexcept
{
    const auto fail = [&]() noexcept {
        reset();
        ready_ = false;
        return PreparedSpectrumFilterBankStatus::InvalidConfiguration;
    };
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || bands.empty()
        || bands.size() > maximumPreparedSpectrumFilterBandCount)
    {
        return fail();
    }
    const auto nyquist = sampleRate * 0.5;
    for (const auto& band : bands)
    {
        if (!std::isfinite(band.amplitude)
            || !std::isfinite(band.centerFrequency)
            || !std::isfinite(band.bandwidth))
        {
            return fail();
        }
        if (band.amplitude != 0.0
            && (band.centerFrequency <= 0.0
                || band.centerFrequency >= nyquist))
        {
            return fail();
        }
    }

    bool unchanged = ready_ && sampleRate_ == sampleRate
        && units_ == units && bandCount_ == bands.size();
    for (std::size_t index = 0; index < bands.size(); ++index)
        unchanged = unchanged && bands_[index] == bands[index];
    if (unchanged)
        return PreparedSpectrumFilterBankStatus::Ok;

    const auto countChanged = bandCount_ != bands.size();
    sampleRate_ = sampleRate;
    units_ = units;
    bandCount_ = bands.size();
    std::ranges::copy(bands, bands_.begin());
    if (countChanged || !ready_)
        reset();
    ready_ = true;
    return PreparedSpectrumFilterBankStatus::Ok;
}

PreparedSpectrumFilterBankStatus PreparedSpectrumFilterBank::process(
    std::span<const double> input,
    std::span<double> output,
    double bandwidthOverride,
    std::span<const double> connectedBandwidth) noexcept
{
    const auto fail = [&](PreparedSpectrumFilterBankStatus status) noexcept {
        std::ranges::fill(output, 0.0);
        reset();
        ready_ = false;
        return status;
    };
    if (!ready_)
        return fail(PreparedSpectrumFilterBankStatus::InvalidConfiguration);
    if (input.size() != output.size()
        || (!connectedBandwidth.empty()
            && connectedBandwidth.size() != input.size()))
    {
        return fail(PreparedSpectrumFilterBankStatus::InvalidBufferShape);
    }
    if (!std::isfinite(bandwidthOverride) || bandwidthOverride < 0.0)
        return fail(PreparedSpectrumFilterBankStatus::InvalidConfiguration);
    for (const auto value : connectedBandwidth)
    {
        if (!std::isfinite(value) || value < 0.0)
            return fail(PreparedSpectrumFilterBankStatus::InvalidConfiguration);
    }

    std::array<Coefficients, maximumPreparedSpectrumFilterBandCount>
        staticCoefficients{};
    const auto validateCoefficients = [&](double overrideValue) noexcept {
        for (std::size_t bandIndex = 0; bandIndex < bandCount_; ++bandIndex)
        {
            const auto& band = bands_[bandIndex];
            if (band.amplitude == 0.0)
                continue;
            const auto bandwidth = spectrumFilterBandwidth(units_, overrideValue, band.bandwidth);
            // A filter of no width passes nothing.
            if (units_ == SpectrumFilterBandwidthUnits::Spacing
                && bandwidth == 0.0)
            {
                continue;
            }
            if (!std::isfinite(bandwidth) || bandwidth <= 0.0
                || !finite(makeBandPassCoefficients(
                    sampleRate_, band.centerFrequency, bandwidth)))
            {
                return false;
            }
        }
        return true;
    };
    if (connectedBandwidth.empty())
    {
        if (!validateCoefficients(bandwidthOverride))
            return fail(PreparedSpectrumFilterBankStatus::NumericFailure);
        for (std::size_t bandIndex = 0; bandIndex < bandCount_; ++bandIndex)
        {
            const auto& band = bands_[bandIndex];
            const auto bandwidth = spectrumFilterBandwidth(units_, bandwidthOverride, band.bandwidth);
            if (band.amplitude == 0.0 || bandwidth == 0.0)
                continue;
            staticCoefficients[bandIndex] = makeBandPassCoefficients(
                sampleRate_, band.centerFrequency, bandwidth);
        }
    }
    else
    {
        for (const auto overrideValue : connectedBandwidth)
        {
            if (!validateCoefficients(overrideValue))
                return fail(PreparedSpectrumFilterBankStatus::NumericFailure);
        }
    }

    std::ranges::fill(output, 0.0);
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (!std::isfinite(input[frame]))
            return fail(PreparedSpectrumFilterBankStatus::NumericFailure);
        const auto overrideValue = connectedBandwidth.empty()
            ? bandwidthOverride : connectedBandwidth[frame];
        for (std::size_t bandIndex = 0; bandIndex < bandCount_; ++bandIndex)
        {
            const auto& band = bands_[bandIndex];
            const auto bandwidth = spectrumFilterBandwidth(units_, overrideValue, band.bandwidth);
            if (band.amplitude == 0.0 || bandwidth == 0.0)
                continue;
            const auto coefficients = connectedBandwidth.empty()
                ? staticCoefficients[bandIndex]
                : makeBandPassCoefficients(
                    sampleRate_, band.centerFrequency, bandwidth);
            const auto contribution = processSample(
                coefficients, states_[bandIndex], input[frame])
                * band.amplitude;
            output[frame] += contribution;
            if (!std::isfinite(contribution)
                || !std::isfinite(output[frame]))
            {
                return fail(PreparedSpectrumFilterBankStatus::NumericFailure);
            }
        }
    }
    return PreparedSpectrumFilterBankStatus::Ok;
}

void PreparedSpectrumFilterBank::reset() noexcept
{
    for (auto& state : states_)
        state = {};
}
}
