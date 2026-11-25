#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace talas::dsp
{
// A Vocoder splits the spectrum into 22 to 88
// bands.
inline constexpr std::size_t maximumLinearVocoderBandCount = 128;
inline constexpr double inheritVocoderEnvelopeTimeSeconds = -1.0;

struct VocoderEnvelopeTimes
{
    double attackSeconds = 0.0;
    double releaseSeconds = 0.0;
};

[[nodiscard]] std::optional<VocoderEnvelopeTimes> resolveVocoderEnvelopeTimes(
    double timeConstantSeconds,
    double attackTimeSeconds,
    double releaseTimeSeconds) noexcept;

enum class VocoderBankFrequencySpacing
{
    Linear,
    Logarithmic,
};

struct VocoderBankFrequencyRange
{
    double lowFrequency = 0.0;
    double highFrequency = 0.0;
};

[[nodiscard]] std::optional<VocoderBankFrequencyRange>
deriveRelativeVocoderBankFrequencyRange(
    double baseFrequency,
    double relativeSpacing,
    std::size_t bandCount,
    VocoderBankFrequencySpacing spacing) noexcept;

// TunableVocoder spacing: hertz between
// linear centers, or half steps between logarithmic ones.
[[nodiscard]] std::optional<VocoderBankFrequencyRange>
deriveVocoderBankFrequencyRange(
    double baseFrequency,
    double spacing,
    std::size_t bandCount,
    VocoderBankFrequencySpacing spacingMode) noexcept;

struct LinearVocoderBankConfiguration
{
    double sampleRate = 0.0;
    std::size_t bandCount = 0;
    double carrierLowFrequency = 0.0;
    double carrierHighFrequency = 0.0;
    double analysisLowFrequency = 0.0;
    double analysisHighFrequency = 0.0;
    double carrierBandwidthRatio = 0.0;
    double analysisBandwidthRatio = 0.0;
    double envelopeTimeConstantSeconds = 0.0;
    VocoderBankFrequencySpacing frequencySpacing =
        VocoderBankFrequencySpacing::Linear;
    double envelopeAttackTimeSeconds = inheritVocoderEnvelopeTimeSeconds;
    double envelopeReleaseTimeSeconds = inheritVocoderEnvelopeTimeSeconds;

    bool operator==(const LinearVocoderBankConfiguration&) const = default;
};

struct MappedVocoderBandConfiguration
{
    double carrierCenterFrequency = 0.0;
    double carrierBandwidth = 0.0;
    double carrierGain = 0.0;
    double analysisCenterFrequency = 0.0;
    double analysisBandwidth = 0.0;
    double analysisGain = 0.0;
};

enum class LinearVocoderBankStatus
{
    Ok,
    InvalidBufferShape,
    NonFiniteInput,
    NumericOverflow,
    InvalidConfiguration,
};

struct LinearVocoderBandState
{
    double carrierFirstIntegrator = 0.0;
    double carrierSecondIntegrator = 0.0;
    double analysisFirstIntegrator = 0.0;
    double analysisSecondIntegrator = 0.0;
    double envelope = 0.0;

    bool operator==(const LinearVocoderBandState&) const = default;
};

struct LinearVocoderBandAuthority
{
    double carrierA1 = 0.0;
    double carrierA2 = 0.0;
    double carrierA3 = 0.0;
    double analysisA1 = 0.0;
    double analysisA2 = 0.0;
    double analysisA3 = 0.0;
    double carrierCenterFrequency = 0.0;
    double analysisCenterFrequency = 0.0;
    double carrierDamping = 0.0;
    double analysisDamping = 0.0;
    double carrierGain = 0.0;
    double analysisGain = 0.0;

    bool operator==(const LinearVocoderBandAuthority&) const = default;
};

struct LinearVocoderBankState
{
    std::size_t bandCount = 0;
    LinearVocoderBankConfiguration configuration;
    double carrierDamping = 0.0;
    double analysisDamping = 0.0;
    double envelopeAttackMemory = 0.0;
    double envelopeReleaseMemory = 0.0;
    bool mappedConfigurationMode = false;
    bool mappedConfigurationReady = false;
    bool preparedCarrierGainsReady = false;
    std::array<LinearVocoderBandState, maximumLinearVocoderBandCount> bands{};
    std::array<LinearVocoderBandAuthority, maximumLinearVocoderBandCount>
        authority{};

    bool operator==(const LinearVocoderBankState&) const = default;
};

class PreparedLinearVocoderBank
{
public:
    explicit PreparedLinearVocoderBank(
        LinearVocoderBankConfiguration configuration);
    PreparedLinearVocoderBank(
        LinearVocoderBankConfiguration configuration,
        std::span<const double> carrierCenterFrequencies,
        std::span<const double> analysisCenterFrequencies);
    PreparedLinearVocoderBank(
        double sampleRate,
        std::size_t bandCount,
        double envelopeTimeConstantSeconds,
        double envelopeAttackTimeSeconds = inheritVocoderEnvelopeTimeSeconds,
        double envelopeReleaseTimeSeconds = inheritVocoderEnvelopeTimeSeconds);

    [[nodiscard]] LinearVocoderBankStatus configureMappedBands(
        std::span<const MappedVocoderBandConfiguration> bands) noexcept;
    [[nodiscard]] LinearVocoderBankStatus configurePreparedCarrierGains(
        std::span<const double> gains) noexcept;
    // Retunes a bank built from a configuration to another of the same
    // sample rate and spacing (and, for explicit centers, new centers),
    // keeping its filters, envelopes and carrier gains, as a hot field's
    // change does. A new band count, up to the maximum the constructor
    // reserves, adds bands at rest or drops the highest without allocating,
    // as a voice that reads NbrBands as it starts needs. InvalidConfiguration,
    // with no change, for one the constructor would refuse or for a mapped
    // bank.
    [[nodiscard]] LinearVocoderBankStatus retune(
        const LinearVocoderBankConfiguration& configuration,
        std::span<const double> carrierCenterFrequencies = {},
        std::span<const double> analysisCenterFrequencies = {}) noexcept;
    // Retunes a bank built from a configuration to the tuning and carrier
    // gains a state of it holds, so that restoreState takes that state.
    [[nodiscard]] LinearVocoderBankStatus retuneTo(
        const LinearVocoderBankState& state) noexcept;

    [[nodiscard]] LinearVocoderBankStatus process(
        std::span<const double> carrier,
        std::span<const double> analysis,
        std::span<double> output) noexcept;
    [[nodiscard]] LinearVocoderBankStatus captureState(
        LinearVocoderBankState& state) const noexcept;
    [[nodiscard]] LinearVocoderBankStatus restoreState(
        const LinearVocoderBankState& state) noexcept;
    void reset() noexcept;

    [[nodiscard]] const LinearVocoderBankConfiguration&
        configuration() const noexcept;
    [[nodiscard]] std::size_t bandCount() const noexcept;
    [[nodiscard]] double envelope(std::size_t bandIndex) const noexcept;
    [[nodiscard]] double carrierCenterFrequency(
        std::size_t bandIndex) const noexcept;
    [[nodiscard]] double analysisCenterFrequency(
        std::size_t bandIndex) const noexcept;

private:
    struct FilterState
    {
        double firstIntegrator = 0.0;
        double secondIntegrator = 0.0;
    };

    struct BandState
    {
        double carrierA1 = 0.0;
        double carrierA2 = 0.0;
        double carrierA3 = 0.0;
        double analysisA1 = 0.0;
        double analysisA2 = 0.0;
        double analysisA3 = 0.0;
        double carrierCenterFrequency = 0.0;
        double analysisCenterFrequency = 0.0;
        double carrierDamping = 0.0;
        double analysisDamping = 0.0;
        double carrierGain = 1.0;
        double analysisGain = 1.0;
        FilterState carrier;
        FilterState analysis;
        double envelope = 0.0;
    };

    struct Tuning
    {
        double carrierDamping = 0.0;
        double analysisDamping = 0.0;
        double envelopeAttackMemory = 0.0;
        double envelopeReleaseMemory = 0.0;
        std::array<BandState, maximumLinearVocoderBandCount> bands{};
    };

    // The constructor's checks and coefficients, without throwing or
    // allocating; false for a configuration the constructor refuses.
    [[nodiscard]] static bool tune(
        const LinearVocoderBankConfiguration& configuration,
        std::span<const double> carrierCenterFrequencies,
        std::span<const double> analysisCenterFrequencies,
        Tuning& tuning) noexcept;

    LinearVocoderBankConfiguration configuration_;
    double carrierDamping_ = 0.0;
    double analysisDamping_ = 0.0;
    double envelopeAttackMemory_ = 0.0;
    double envelopeReleaseMemory_ = 0.0;
    std::vector<BandState> bands_;
    bool mappedConfigurationMode_ = false;
    bool mappedConfigurationReady_ = true;
    bool preparedCarrierGainsReady_ = true;

    void resetState() noexcept;
    void resetMappedConfiguration() noexcept;
};
}
