#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace talas::dsp
{
inline constexpr std::size_t maximumPreparedSpectrumFilterBandCount = 64;

enum class PreparedSpectrumFilterBankStatus
{
    Ok,
    InvalidConfiguration,
    InvalidBufferShape,
    NumericFailure,
};

// How a FilterBank's Bandwidth sets the width of its filters. It is a 0 to 1
// control: each filter is that fraction of its spacing from the nearer
// neighbouring partial wide, given in a band's bandwidth. Projects before
// format 58 gave hertz, where 0 took the Spectrum's own bandwidths.
enum class SpectrumFilterBandwidthUnits
{
    Spacing,
    LegacyHertz,
};

// The distance from a partial to the nearer of its neighbours in the
// Spectrum, or the partial's own frequency when it has no neighbour at a
// different frequency; a missing neighbour is given as a non-finite value.
[[nodiscard]] double spectrumFilterSpacing(
    double below,
    double center,
    double above) noexcept;

// The width in hertz of a filter under the Bandwidth control, given the
// filter's spacing, or under hertz units the Spectrum's bandwidth. A width of
// 0 under spacing units passes nothing.
[[nodiscard]] double spectrumFilterBandwidth(
    SpectrumFilterBandwidthUnits units,
    double control,
    double width) noexcept;

struct SpectrumFilterBandConfiguration
{
    double amplitude = 0.0;
    double centerFrequency = 0.0;
    double bandwidth = 0.0;

    [[nodiscard]] bool operator==(
        const SpectrumFilterBandConfiguration&) const noexcept = default;
};

class PreparedSpectrumFilterBank
{
public:
    [[nodiscard]] PreparedSpectrumFilterBankStatus configure(
        double sampleRate,
        std::span<const SpectrumFilterBandConfiguration> bands,
        SpectrumFilterBandwidthUnits units) noexcept;

    [[nodiscard]] PreparedSpectrumFilterBankStatus process(
        std::span<const double> input,
        std::span<double> output,
        double bandwidthOverride,
        std::span<const double> connectedBandwidth = {}) noexcept;

    void reset() noexcept;

    [[nodiscard]] std::size_t bandCount() const noexcept { return bandCount_; }
    [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
    struct BandState
    {
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;
    };

    double sampleRate_ = 0.0;
    SpectrumFilterBandwidthUnits units_ = SpectrumFilterBandwidthUnits::Spacing;
    std::array<SpectrumFilterBandConfiguration,
        maximumPreparedSpectrumFilterBandCount> bands_{};
    std::array<BandState, maximumPreparedSpectrumFilterBandCount> states_{};
    std::size_t bandCount_ = 0;
    bool ready_ = false;
};
}
