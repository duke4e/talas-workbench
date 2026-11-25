#include "talas/dsp/LinearVocoderBank.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] bool validFrequencyRange(
    double low,
    double high,
    double nyquist) noexcept
{
    return std::isfinite(low) && std::isfinite(high)
        && low > 0.0 && low <= high && high < nyquist;
}

[[nodiscard]] double bandFrequency(
    double low,
    double high,
    std::size_t index,
    std::size_t count,
    VocoderBankFrequencySpacing spacing) noexcept
{
    if (count == 1)
        return low;
    switch (spacing)
    {
        case VocoderBankFrequencySpacing::Linear:
            return low + (high - low) * static_cast<double>(index)
                / static_cast<double>(count - 1);
        case VocoderBankFrequencySpacing::Logarithmic:
        {
            const auto fraction = static_cast<double>(index)
                / static_cast<double>(count - 1);
            return std::exp(
                std::log(low) + (std::log(high) - std::log(low))
                    * fraction);
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] bool coefficients(
    double frequency,
    double sampleRate,
    double damping,
    double& a1,
    double& a2,
    double& a3) noexcept
{
    const auto g = std::tan(std::numbers::pi * frequency / sampleRate);
    const auto denominator = 1.0 + damping * g + g * g;
    if (!std::isfinite(g) || g <= 0.0
        || !std::isfinite(denominator) || denominator <= 0.0)
    {
        return false;
    }
    a1 = 1.0 / denominator;
    a2 = g * a1;
    a3 = g * a2;
    return std::isfinite(a1) && std::isfinite(a2) && std::isfinite(a3);
}

void prepareCoefficients(
    double frequency,
    double sampleRate,
    double damping,
    double& a1,
    double& a2,
    double& a3)
{
    if (!coefficients(frequency, sampleRate, damping, a1, a2, a3))
    {
        throw std::invalid_argument(
            "Invalid linear-vocoder filter coefficients");
    }
}

[[nodiscard]] double processBandPass(
    double input,
    double damping,
    double a1,
    double a2,
    double a3,
    auto& state) noexcept
{
    const auto difference = input - state.secondIntegrator;
    const auto bandPass = a1 * state.firstIntegrator + a2 * difference;
    const auto lowPass = state.secondIntegrator
        + a2 * state.firstIntegrator + a3 * difference;
    state.firstIntegrator = 2.0 * bandPass - state.firstIntegrator;
    state.secondIntegrator = 2.0 * lowPass - state.secondIntegrator;
    return bandPass * damping;
}

[[nodiscard]] std::optional<double> envelopeMemory(
    double sampleRate,
    double timeSeconds) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(timeSeconds) || timeSeconds < 0.0)
    {
        return std::nullopt;
    }
    const auto memory = timeSeconds == 0.0
        ? 0.0 : std::exp(-1.0 / (sampleRate * timeSeconds));
    if (!std::isfinite(memory) || memory < 0.0 || memory >= 1.0)
        return std::nullopt;
    return memory;
}
}

std::optional<VocoderEnvelopeTimes> resolveVocoderEnvelopeTimes(
    double timeConstantSeconds,
    double attackTimeSeconds,
    double releaseTimeSeconds) noexcept
{
    const auto validOverride = [](double value) noexcept {
        return std::isfinite(value)
            && (value == inheritVocoderEnvelopeTimeSeconds || value >= 0.0);
    };
    if (!std::isfinite(timeConstantSeconds) || timeConstantSeconds < 0.0
        || !validOverride(attackTimeSeconds)
        || !validOverride(releaseTimeSeconds))
    {
        return std::nullopt;
    }
    return VocoderEnvelopeTimes{
        attackTimeSeconds >= 0.0
            ? attackTimeSeconds : timeConstantSeconds,
        releaseTimeSeconds >= 0.0
            ? releaseTimeSeconds : timeConstantSeconds,
    };
}

std::optional<VocoderBankFrequencyRange>
deriveRelativeVocoderBankFrequencyRange(
    double baseFrequency,
    double relativeSpacing,
    std::size_t bandCount,
    VocoderBankFrequencySpacing spacing) noexcept
{
    if (!std::isfinite(baseFrequency) || baseFrequency <= 0.0
        || !std::isfinite(relativeSpacing) || relativeSpacing <= 0.0
        || bandCount == 0)
    {
        return std::nullopt;
    }
    if (bandCount == 1)
        return VocoderBankFrequencyRange{baseFrequency, baseFrequency};

    const auto intervals = static_cast<double>(bandCount - 1);
    double highFrequency = 0.0;
    switch (spacing)
    {
        case VocoderBankFrequencySpacing::Linear:
            highFrequency = baseFrequency
                * (1.0 + relativeSpacing * intervals);
            break;
        case VocoderBankFrequencySpacing::Logarithmic:
            highFrequency = baseFrequency
                * std::exp2(relativeSpacing * intervals);
            break;
        default:
            return std::nullopt;
    }
    if (!std::isfinite(highFrequency) || highFrequency < baseFrequency)
        return std::nullopt;
    return VocoderBankFrequencyRange{baseFrequency, highFrequency};
}

std::optional<VocoderBankFrequencyRange>
deriveVocoderBankFrequencyRange(
    double baseFrequency,
    double spacing,
    std::size_t bandCount,
    VocoderBankFrequencySpacing spacingMode) noexcept
{
    if (!std::isfinite(baseFrequency) || baseFrequency <= 0.0
        || !std::isfinite(spacing) || spacing <= 0.0
        || bandCount == 0)
    {
        return std::nullopt;
    }
    const auto intervals = static_cast<double>(bandCount - 1);
    double highFrequency = 0.0;
    switch (spacingMode)
    {
        case VocoderBankFrequencySpacing::Linear:
            highFrequency = baseFrequency + spacing * intervals;
            break;
        case VocoderBankFrequencySpacing::Logarithmic:
            highFrequency = baseFrequency
                * std::exp2(spacing * intervals / 12.0);
            break;
        default:
            return std::nullopt;
    }
    if (!std::isfinite(highFrequency) || highFrequency < baseFrequency)
        return std::nullopt;
    return VocoderBankFrequencyRange{baseFrequency, highFrequency};
}

PreparedLinearVocoderBank::PreparedLinearVocoderBank(
    LinearVocoderBankConfiguration configuration)
    : PreparedLinearVocoderBank(configuration, {}, {})
{
}

bool PreparedLinearVocoderBank::tune(
    const LinearVocoderBankConfiguration& configuration,
    std::span<const double> carrierCenterFrequencies,
    std::span<const double> analysisCenterFrequencies,
    Tuning& tuning) noexcept
{
    const auto nyquist = configuration.sampleRate * 0.5;
    const auto explicitCenters = !carrierCenterFrequencies.empty()
        || !analysisCenterFrequencies.empty();
    if (!std::isfinite(configuration.sampleRate)
        || configuration.sampleRate <= 0.0
        || configuration.bandCount == 0
        || configuration.bandCount > maximumLinearVocoderBandCount
        || (explicitCenters
            && (carrierCenterFrequencies.size() != configuration.bandCount
                || analysisCenterFrequencies.size()
                    != configuration.bandCount))
        || (!explicitCenters
            && (!validFrequencyRange(
                    configuration.carrierLowFrequency,
                    configuration.carrierHighFrequency,
                    nyquist)
                || !validFrequencyRange(
                    configuration.analysisLowFrequency,
                    configuration.analysisHighFrequency,
                    nyquist)))
        || !std::isfinite(configuration.carrierBandwidthRatio)
        || configuration.carrierBandwidthRatio <= 0.0
        || !std::isfinite(configuration.analysisBandwidthRatio)
        || configuration.analysisBandwidthRatio <= 0.0
        || !resolveVocoderEnvelopeTimes(
            configuration.envelopeTimeConstantSeconds,
            configuration.envelopeAttackTimeSeconds,
            configuration.envelopeReleaseTimeSeconds).has_value())
    {
        return false;
    }

    tuning.carrierDamping = configuration.carrierBandwidthRatio;
    tuning.analysisDamping = configuration.analysisBandwidthRatio;
    const auto envelopeTimes = *resolveVocoderEnvelopeTimes(
        configuration.envelopeTimeConstantSeconds,
        configuration.envelopeAttackTimeSeconds,
        configuration.envelopeReleaseTimeSeconds);
    const auto attackMemory = envelopeMemory(
        configuration.sampleRate, envelopeTimes.attackSeconds);
    const auto releaseMemory = envelopeMemory(
        configuration.sampleRate, envelopeTimes.releaseSeconds);
    if (!attackMemory.has_value() || !releaseMemory.has_value())
        return false;
    tuning.envelopeAttackMemory = *attackMemory;
    tuning.envelopeReleaseMemory = *releaseMemory;

    const auto count = configuration.bandCount;
    auto& bands = tuning.bands;
    for (std::size_t index = 0; index < count; ++index)
    {
        auto& band = bands[index];
        band = BandState{};
        band.carrierCenterFrequency = explicitCenters
            ? carrierCenterFrequencies[index]
            : bandFrequency(
                configuration.carrierLowFrequency,
                configuration.carrierHighFrequency,
                index,
                count,
                configuration.frequencySpacing);
        band.analysisCenterFrequency = explicitCenters
            ? analysisCenterFrequencies[index]
            : bandFrequency(
                configuration.analysisLowFrequency,
                configuration.analysisHighFrequency,
                index,
                count,
                configuration.frequencySpacing);
        if (!std::isfinite(band.carrierCenterFrequency)
            || band.carrierCenterFrequency <= 0.0
            || band.carrierCenterFrequency >= nyquist
            || !std::isfinite(band.analysisCenterFrequency)
            || band.analysisCenterFrequency <= 0.0
            || band.analysisCenterFrequency >= nyquist
            || (index != 0
                && (band.carrierCenterFrequency
                        <= bands[index - 1].carrierCenterFrequency
                    || band.analysisCenterFrequency
                        <= bands[index - 1].analysisCenterFrequency)))
        {
            return false;
        }
    }

    // A band is as wide as the ratio times its spacing from its nearest
    // neighbour, so the bank divides the spectrum into regions and each band
    // measures its own; a lone band takes the
    // ratio as its damping. The damping of a state-variable band pass is its
    // bandwidth over its center frequency.
    const auto spacingDamping = [&](auto center, std::size_t index, double ratio) {
        if (count == 1)
            return ratio;
        auto spacing = std::numeric_limits<double>::infinity();
        if (index != 0)
            spacing = std::min(spacing, center(index) - center(index - 1));
        if (index + 1 != count)
            spacing = std::min(spacing, center(index + 1) - center(index));
        return ratio * spacing / center(index);
    };
    const auto carrierCenter = [&](std::size_t index) {
        return bands[index].carrierCenterFrequency;
    };
    const auto analysisCenter = [&](std::size_t index) {
        return bands[index].analysisCenterFrequency;
    };
    for (std::size_t index = 0; index < count; ++index)
    {
        auto& band = bands[index];
        band.carrierDamping = spacingDamping(carrierCenter, index, tuning.carrierDamping);
        band.analysisDamping = spacingDamping(analysisCenter, index, tuning.analysisDamping);
        if (!std::isfinite(band.carrierDamping) || band.carrierDamping <= 0.0
            || !std::isfinite(band.analysisDamping) || band.analysisDamping <= 0.0
            || !coefficients(
                band.carrierCenterFrequency,
                configuration.sampleRate,
                band.carrierDamping,
                band.carrierA1,
                band.carrierA2,
                band.carrierA3)
            || !coefficients(
                band.analysisCenterFrequency,
                configuration.sampleRate,
                band.analysisDamping,
                band.analysisA1,
                band.analysisA2,
                band.analysisA3))
        {
            return false;
        }
    }
    return true;
}

PreparedLinearVocoderBank::PreparedLinearVocoderBank(
    LinearVocoderBankConfiguration configuration,
    std::span<const double> carrierCenterFrequencies,
    std::span<const double> analysisCenterFrequencies)
    : configuration_(configuration)
{
    const auto tuning = std::make_unique<Tuning>();
    if (!tune(configuration_, carrierCenterFrequencies,
            analysisCenterFrequencies, *tuning))
    {
        throw std::invalid_argument(
            "Invalid linear-vocoder bank configuration");
    }
    carrierDamping_ = tuning->carrierDamping;
    analysisDamping_ = tuning->analysisDamping;
    envelopeAttackMemory_ = tuning->envelopeAttackMemory;
    envelopeReleaseMemory_ = tuning->envelopeReleaseMemory;
    // Room for every band a retune may give it, so that one changing the
    // band count never allocates.
    bands_.reserve(maximumLinearVocoderBandCount);
    bands_.assign(tuning->bands.begin(),
        tuning->bands.begin() + static_cast<std::ptrdiff_t>(configuration_.bandCount));
}

LinearVocoderBankStatus PreparedLinearVocoderBank::retune(
    const LinearVocoderBankConfiguration& configuration,
    std::span<const double> carrierCenterFrequencies,
    std::span<const double> analysisCenterFrequencies) noexcept
{
    if (mappedConfigurationMode_
        || configuration.sampleRate != configuration_.sampleRate
        || configuration.bandCount == 0
        || configuration.bandCount > bands_.capacity()
        || configuration.frequencySpacing != configuration_.frequencySpacing)
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    Tuning tuning;
    if (!tune(configuration, carrierCenterFrequencies,
            analysisCenterFrequencies, tuning))
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    // A new band count, within the bands reserved, adds bands at rest (or
    // drops the highest); the rest keep their filters and envelopes.
    bands_.resize(configuration.bandCount);
    // The integrator filters, envelopes and carrier gains stay valid across
    // coefficient changes.
    configuration_ = configuration;
    carrierDamping_ = tuning.carrierDamping;
    analysisDamping_ = tuning.analysisDamping;
    envelopeAttackMemory_ = tuning.envelopeAttackMemory;
    envelopeReleaseMemory_ = tuning.envelopeReleaseMemory;
    for (std::size_t index = 0; index < bands_.size(); ++index)
    {
        const auto& source = tuning.bands[index];
        auto& destination = bands_[index];
        destination.carrierA1 = source.carrierA1;
        destination.carrierA2 = source.carrierA2;
        destination.carrierA3 = source.carrierA3;
        destination.analysisA1 = source.analysisA1;
        destination.analysisA2 = source.analysisA2;
        destination.analysisA3 = source.analysisA3;
        destination.carrierCenterFrequency = source.carrierCenterFrequency;
        destination.analysisCenterFrequency = source.analysisCenterFrequency;
        destination.carrierDamping = source.carrierDamping;
        destination.analysisDamping = source.analysisDamping;
    }
    return LinearVocoderBankStatus::Ok;
}

LinearVocoderBankStatus PreparedLinearVocoderBank::retuneTo(
    const LinearVocoderBankState& state) noexcept
{
    if (mappedConfigurationMode_ || state.bandCount > bands_.capacity()
        || state.bandCount == 0 || state.bandCount > maximumLinearVocoderBandCount)
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    std::array<double, maximumLinearVocoderBandCount> carrierCenters{};
    std::array<double, maximumLinearVocoderBandCount> analysisCenters{};
    std::array<double, maximumLinearVocoderBandCount> gains{};
    for (std::size_t index = 0; index < state.bandCount; ++index)
    {
        carrierCenters[index] = state.authority[index].carrierCenterFrequency;
        analysisCenters[index] = state.authority[index].analysisCenterFrequency;
        gains[index] = state.authority[index].carrierGain;
    }
    if (state.configuration == configuration_
        && std::ranges::equal(std::span{carrierCenters}.first(state.bandCount),
            bands_, {}, {}, &BandState::carrierCenterFrequency)
        && std::ranges::equal(std::span{analysisCenters}.first(state.bandCount),
            bands_, {}, {}, &BandState::analysisCenterFrequency))
    {
        return configurePreparedCarrierGains(std::span{gains}.first(state.bandCount));
    }
    const auto status = retune(state.configuration,
        std::span{carrierCenters}.first(state.bandCount),
        std::span{analysisCenters}.first(state.bandCount));
    if (status != LinearVocoderBankStatus::Ok)
        return status;
    return configurePreparedCarrierGains(std::span{gains}.first(state.bandCount));
}

PreparedLinearVocoderBank::PreparedLinearVocoderBank(
    double sampleRate,
    std::size_t bandCount,
    double envelopeTimeConstantSeconds,
    double envelopeAttackTimeSeconds,
    double envelopeReleaseTimeSeconds)
    : configuration_{
        sampleRate,
        bandCount,
        0.0,
        0.0,
        0.0,
        0.0,
        1.0,
        1.0,
        envelopeTimeConstantSeconds,
        VocoderBankFrequencySpacing::Linear,
        envelopeAttackTimeSeconds,
        envelopeReleaseTimeSeconds}
    , mappedConfigurationMode_(true)
    , mappedConfigurationReady_(false)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || bandCount == 0 || bandCount > maximumLinearVocoderBandCount
        || !resolveVocoderEnvelopeTimes(
            envelopeTimeConstantSeconds,
            envelopeAttackTimeSeconds,
            envelopeReleaseTimeSeconds).has_value())
    {
        throw std::invalid_argument(
            "Invalid mapped-vocoder bank configuration");
    }
    const auto envelopeTimes = *resolveVocoderEnvelopeTimes(
        envelopeTimeConstantSeconds,
        envelopeAttackTimeSeconds,
        envelopeReleaseTimeSeconds);
    const auto attackMemory = envelopeMemory(
        sampleRate, envelopeTimes.attackSeconds);
    const auto releaseMemory = envelopeMemory(
        sampleRate, envelopeTimes.releaseSeconds);
    if (!attackMemory.has_value() || !releaseMemory.has_value())
    {
        throw std::invalid_argument(
            "Invalid mapped-vocoder envelope coefficient");
    }
    envelopeAttackMemory_ = *attackMemory;
    envelopeReleaseMemory_ = *releaseMemory;
    bands_.resize(bandCount);
}

LinearVocoderBankStatus PreparedLinearVocoderBank::configureMappedBands(
    std::span<const MappedVocoderBandConfiguration> configurations) noexcept
{
    struct Candidate
    {
        MappedVocoderBandConfiguration configuration;
        double carrierDamping = 0.0;
        double analysisDamping = 0.0;
        double carrierA1 = 0.0;
        double carrierA2 = 0.0;
        double carrierA3 = 0.0;
        double analysisA1 = 0.0;
        double analysisA2 = 0.0;
        double analysisA3 = 0.0;
    };
    std::array<Candidate, maximumLinearVocoderBandCount> candidates{};
    const auto fail = [&]() noexcept {
        resetMappedConfiguration();
        return LinearVocoderBankStatus::InvalidConfiguration;
    };
    if (!mappedConfigurationMode_)
    {
        resetState();
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    if (configurations.size() != bands_.size())
    {
        return fail();
    }
    const auto nyquist = configuration_.sampleRate * 0.5;
    bool unchanged = mappedConfigurationReady_;
    for (std::size_t index = 0; index < configurations.size(); ++index)
    {
        const auto& source = configurations[index];
        if (!std::isfinite(source.carrierCenterFrequency)
            || source.carrierCenterFrequency <= 0.0
            || source.carrierCenterFrequency >= nyquist
            || !std::isfinite(source.analysisCenterFrequency)
            || source.analysisCenterFrequency <= 0.0
            || source.analysisCenterFrequency >= nyquist
            || !std::isfinite(source.carrierBandwidth)
            || source.carrierBandwidth <= 0.0
            || !std::isfinite(source.analysisBandwidth)
            || source.analysisBandwidth <= 0.0
            || !std::isfinite(source.carrierGain)
            || source.carrierGain < 0.0
            || !std::isfinite(source.analysisGain)
            || source.analysisGain < 0.0
            || (index != 0
                && (source.carrierCenterFrequency
                        <= configurations[index - 1].carrierCenterFrequency
                    || source.analysisCenterFrequency
                        <= configurations[index - 1].analysisCenterFrequency)))
        {
            return fail();
        }
        auto& candidate = candidates[index];
        candidate.configuration = source;
        candidate.carrierDamping = source.carrierBandwidth
            / source.carrierCenterFrequency;
        candidate.analysisDamping = source.analysisBandwidth
            / source.analysisCenterFrequency;
        if (!std::isfinite(candidate.carrierDamping)
            || candidate.carrierDamping <= 0.0
            || !std::isfinite(candidate.analysisDamping)
            || candidate.analysisDamping <= 0.0)
        {
            return fail();
        }
        try
        {
            prepareCoefficients(
                source.carrierCenterFrequency,
                configuration_.sampleRate,
                candidate.carrierDamping,
                candidate.carrierA1,
                candidate.carrierA2,
                candidate.carrierA3);
            prepareCoefficients(
                source.analysisCenterFrequency,
                configuration_.sampleRate,
                candidate.analysisDamping,
                candidate.analysisA1,
                candidate.analysisA2,
                candidate.analysisA3);
        }
        catch (...)
        {
            return fail();
        }
        const auto& current = bands_[index];
        unchanged = unchanged
            && current.carrierCenterFrequency
                == source.carrierCenterFrequency
            && current.analysisCenterFrequency
                == source.analysisCenterFrequency
            && current.carrierDamping == candidate.carrierDamping
            && current.analysisDamping == candidate.analysisDamping
            && current.carrierGain == source.carrierGain
            && current.analysisGain == source.analysisGain;
    }
    if (unchanged)
        return LinearVocoderBankStatus::Ok;

    // The integrator filters and envelopes stay valid across coefficient
    // and gain changes; only a first configuration starts from silence.
    const auto firstConfiguration = !mappedConfigurationReady_;
    for (std::size_t index = 0; index < bands_.size(); ++index)
    {
        const auto& candidate = candidates[index];
        auto& destination = bands_[index];
        destination.carrierCenterFrequency =
            candidate.configuration.carrierCenterFrequency;
        destination.analysisCenterFrequency =
            candidate.configuration.analysisCenterFrequency;
        destination.carrierDamping = candidate.carrierDamping;
        destination.analysisDamping = candidate.analysisDamping;
        destination.carrierGain = candidate.configuration.carrierGain;
        destination.analysisGain = candidate.configuration.analysisGain;
        destination.carrierA1 = candidate.carrierA1;
        destination.carrierA2 = candidate.carrierA2;
        destination.carrierA3 = candidate.carrierA3;
        destination.analysisA1 = candidate.analysisA1;
        destination.analysisA2 = candidate.analysisA2;
        destination.analysisA3 = candidate.analysisA3;
    }
    if (firstConfiguration)
        resetState();
    mappedConfigurationReady_ = true;
    return LinearVocoderBankStatus::Ok;
}

LinearVocoderBankStatus
PreparedLinearVocoderBank::configurePreparedCarrierGains(
    std::span<const double> gains) noexcept
{
    if (mappedConfigurationMode_)
    {
        resetMappedConfiguration();
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    const auto fail = [&]() noexcept {
        resetState();
        preparedCarrierGainsReady_ = false;
        return LinearVocoderBankStatus::InvalidConfiguration;
    };
    if (gains.size() != bands_.size())
        return fail();
    bool unchanged = preparedCarrierGainsReady_;
    for (std::size_t index = 0; index < gains.size(); ++index)
    {
        if (!std::isfinite(gains[index]) || gains[index] < 0.0)
            return fail();
        unchanged = unchanged && bands_[index].carrierGain == gains[index];
    }
    if (unchanged)
        return LinearVocoderBankStatus::Ok;
    for (std::size_t index = 0; index < gains.size(); ++index)
        bands_[index].carrierGain = gains[index];
    // A gain change keeps the filters and envelopes; only the first
    // publication starts from silence.
    if (!preparedCarrierGainsReady_)
        resetState();
    preparedCarrierGainsReady_ = true;
    return LinearVocoderBankStatus::Ok;
}

LinearVocoderBankStatus PreparedLinearVocoderBank::process(
    std::span<const double> carrier,
    std::span<const double> analysis,
    std::span<double> output) noexcept
{
    const auto fail = [&](LinearVocoderBankStatus status) noexcept {
        reset();
        std::fill(output.begin(), output.end(), 0.0);
        return status;
    };
    if (carrier.size() != output.size() || analysis.size() != output.size())
        return fail(LinearVocoderBankStatus::InvalidBufferShape);
    if (mappedConfigurationMode_ && !mappedConfigurationReady_)
        return fail(LinearVocoderBankStatus::InvalidConfiguration);
    if (!mappedConfigurationMode_ && !preparedCarrierGainsReady_)
        return fail(LinearVocoderBankStatus::InvalidConfiguration);

    const auto outputGain = 1.0 / static_cast<double>(bands_.size());
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        if (!std::isfinite(carrier[frame])
            || !std::isfinite(analysis[frame]))
        {
            return fail(LinearVocoderBankStatus::NonFiniteInput);
        }

        double sum = 0.0;
        for (auto& band : bands_)
        {
            const auto carrierBand = processBandPass(
                carrier[frame], band.carrierDamping, band.carrierA1, band.carrierA2,
                band.carrierA3, band.carrier);
            const auto analysisBand = processBandPass(
                analysis[frame] * band.analysisGain,
                band.analysisDamping, band.analysisA1, band.analysisA2,
                band.analysisA3, band.analysis);
            const auto envelopeTarget = std::abs(analysisBand);
            const auto envelopeMemory = envelopeTarget > band.envelope
                ? envelopeAttackMemory_ : envelopeReleaseMemory_;
            band.envelope = envelopeMemory * band.envelope
                + (1.0 - envelopeMemory) * envelopeTarget;
            const auto contribution = carrierBand * band.carrierGain
                * band.envelope;
            sum += contribution;
            if (!std::isfinite(carrierBand)
                || !std::isfinite(analysisBand)
                || !std::isfinite(band.envelope)
                || band.envelope < 0.0
                || !std::isfinite(contribution)
                || !std::isfinite(sum))
            {
                return fail(LinearVocoderBankStatus::NumericOverflow);
            }
        }
        output[frame] = sum * outputGain;
        if (!std::isfinite(output[frame]))
            return fail(LinearVocoderBankStatus::NumericOverflow);
    }
    return LinearVocoderBankStatus::Ok;
}

LinearVocoderBankStatus PreparedLinearVocoderBank::captureState(
    LinearVocoderBankState& state) const noexcept
{
    if (bands_.empty()
        || bands_.size() > maximumLinearVocoderBandCount)
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    for (const auto& band : bands_)
    {
        if (!std::isfinite(band.carrier.firstIntegrator)
            || !std::isfinite(band.carrier.secondIntegrator)
            || !std::isfinite(band.analysis.firstIntegrator)
            || !std::isfinite(band.analysis.secondIntegrator)
            || !std::isfinite(band.envelope)
            || band.envelope < 0.0)
        {
            return LinearVocoderBankStatus::InvalidConfiguration;
        }
    }

    state = {};
    state.bandCount = bands_.size();
    state.configuration = configuration_;
    state.carrierDamping = carrierDamping_;
    state.analysisDamping = analysisDamping_;
    state.envelopeAttackMemory = envelopeAttackMemory_;
    state.envelopeReleaseMemory = envelopeReleaseMemory_;
    state.mappedConfigurationMode = mappedConfigurationMode_;
    state.mappedConfigurationReady = mappedConfigurationReady_;
    state.preparedCarrierGainsReady = preparedCarrierGainsReady_;
    for (std::size_t index = 0; index < bands_.size(); ++index)
    {
        const auto& source = bands_[index];
        state.bands[index] = {
            source.carrier.firstIntegrator,
            source.carrier.secondIntegrator,
            source.analysis.firstIntegrator,
            source.analysis.secondIntegrator,
            source.envelope,
        };
        state.authority[index] = {
            source.carrierA1,
            source.carrierA2,
            source.carrierA3,
            source.analysisA1,
            source.analysisA2,
            source.analysisA3,
            source.carrierCenterFrequency,
            source.analysisCenterFrequency,
            source.carrierDamping,
            source.analysisDamping,
            source.carrierGain,
            source.analysisGain,
        };
    }
    return LinearVocoderBankStatus::Ok;
}

LinearVocoderBankStatus PreparedLinearVocoderBank::restoreState(
    const LinearVocoderBankState& state) noexcept
{
    if (state.bandCount != bands_.size()
        || state.bandCount == 0
        || state.bandCount > maximumLinearVocoderBandCount
        || state.configuration != configuration_
        || state.carrierDamping != carrierDamping_
        || state.analysisDamping != analysisDamping_
        || state.envelopeAttackMemory != envelopeAttackMemory_
        || state.envelopeReleaseMemory != envelopeReleaseMemory_
        || state.mappedConfigurationMode != mappedConfigurationMode_
        || state.preparedCarrierGainsReady
            != preparedCarrierGainsReady_
        || (!mappedConfigurationMode_
            && state.mappedConfigurationReady
                != mappedConfigurationReady_))
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    bool mappedAuthorityIsUnconfigured = mappedConfigurationMode_;
    const auto nyquist = configuration_.sampleRate * 0.5;
    for (std::size_t index = 0; index < state.bandCount; ++index)
    {
        const auto& band = state.bands[index];
        const auto& authority = state.authority[index];
        const auto& current = bands_[index];
        if (!std::isfinite(band.carrierFirstIntegrator)
            || !std::isfinite(band.carrierSecondIntegrator)
            || !std::isfinite(band.analysisFirstIntegrator)
            || !std::isfinite(band.analysisSecondIntegrator)
            || !std::isfinite(band.envelope)
            || band.envelope < 0.0
            || (mappedConfigurationMode_
                && !state.mappedConfigurationReady
                && (band.carrierFirstIntegrator != 0.0
                    || band.carrierSecondIntegrator != 0.0
                    || band.analysisFirstIntegrator != 0.0
                    || band.analysisSecondIntegrator != 0.0
                    || band.envelope != 0.0)))
        {
            return LinearVocoderBankStatus::InvalidConfiguration;
        }
        if (!mappedConfigurationMode_)
        {
            if (authority.carrierA1 != current.carrierA1
                || authority.carrierA2 != current.carrierA2
                || authority.carrierA3 != current.carrierA3
                || authority.analysisA1 != current.analysisA1
                || authority.analysisA2 != current.analysisA2
                || authority.analysisA3 != current.analysisA3
                || authority.carrierCenterFrequency
                    != current.carrierCenterFrequency
                || authority.analysisCenterFrequency
                    != current.analysisCenterFrequency
                || authority.carrierDamping != current.carrierDamping
                || authority.analysisDamping != current.analysisDamping
                || authority.carrierGain != current.carrierGain
                || authority.analysisGain != current.analysisGain)
            {
                return LinearVocoderBankStatus::InvalidConfiguration;
            }
            continue;
        }

        const auto unconfigured = authority.carrierA1 == 0.0
            && authority.carrierA2 == 0.0
            && authority.carrierA3 == 0.0
            && authority.analysisA1 == 0.0
            && authority.analysisA2 == 0.0
            && authority.analysisA3 == 0.0
            && authority.carrierCenterFrequency == 0.0
            && authority.analysisCenterFrequency == 0.0
            && authority.carrierDamping == 0.0
            && authority.analysisDamping == 0.0
            && authority.carrierGain == 1.0
            && authority.analysisGain == 1.0;
        if (unconfigured)
            continue;
        mappedAuthorityIsUnconfigured = false;

        if (!std::isfinite(authority.carrierCenterFrequency)
            || authority.carrierCenterFrequency <= 0.0
            || authority.carrierCenterFrequency >= nyquist
            || !std::isfinite(authority.analysisCenterFrequency)
            || authority.analysisCenterFrequency <= 0.0
            || authority.analysisCenterFrequency >= nyquist
            || !std::isfinite(authority.carrierDamping)
            || authority.carrierDamping <= 0.0
            || !std::isfinite(authority.analysisDamping)
            || authority.analysisDamping <= 0.0
            || !std::isfinite(authority.carrierGain)
            || authority.carrierGain < 0.0
            || !std::isfinite(authority.analysisGain)
            || authority.analysisGain < 0.0
            || (index != 0
                && (authority.carrierCenterFrequency
                        <= state.authority[index - 1]
                               .carrierCenterFrequency
                    || authority.analysisCenterFrequency
                        <= state.authority[index - 1]
                               .analysisCenterFrequency)))
        {
            return LinearVocoderBankStatus::InvalidConfiguration;
        }
        double carrierA1 = 0.0;
        double carrierA2 = 0.0;
        double carrierA3 = 0.0;
        double analysisA1 = 0.0;
        double analysisA2 = 0.0;
        double analysisA3 = 0.0;
        try
        {
            prepareCoefficients(
                authority.carrierCenterFrequency,
                configuration_.sampleRate,
                authority.carrierDamping,
                carrierA1,
                carrierA2,
                carrierA3);
            prepareCoefficients(
                authority.analysisCenterFrequency,
                configuration_.sampleRate,
                authority.analysisDamping,
                analysisA1,
                analysisA2,
                analysisA3);
        }
        catch (...)
        {
            return LinearVocoderBankStatus::InvalidConfiguration;
        }
        if (authority.carrierA1 != carrierA1
            || authority.carrierA2 != carrierA2
            || authority.carrierA3 != carrierA3
            || authority.analysisA1 != analysisA1
            || authority.analysisA2 != analysisA2
            || authority.analysisA3 != analysisA3)
        {
            return LinearVocoderBankStatus::InvalidConfiguration;
        }
    }
    if (mappedConfigurationMode_
        && ((mappedAuthorityIsUnconfigured
                && state.mappedConfigurationReady)
            || (!mappedAuthorityIsUnconfigured
                && !state.mappedConfigurationReady)
            || (!mappedAuthorityIsUnconfigured
                && std::ranges::any_of(
                    std::span{state.authority}.first(state.bandCount),
                    [](const auto& authority) {
                        return authority.carrierCenterFrequency == 0.0;
                    }))))
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }

    for (std::size_t index = 0; index < state.bandCount; ++index)
    {
        const auto& source = state.bands[index];
        const auto& authority = state.authority[index];
        auto& destination = bands_[index];
        if (mappedConfigurationMode_)
        {
            destination.carrierA1 = authority.carrierA1;
            destination.carrierA2 = authority.carrierA2;
            destination.carrierA3 = authority.carrierA3;
            destination.analysisA1 = authority.analysisA1;
            destination.analysisA2 = authority.analysisA2;
            destination.analysisA3 = authority.analysisA3;
            destination.carrierCenterFrequency =
                authority.carrierCenterFrequency;
            destination.analysisCenterFrequency =
                authority.analysisCenterFrequency;
            destination.carrierDamping = authority.carrierDamping;
            destination.analysisDamping = authority.analysisDamping;
            destination.carrierGain = authority.carrierGain;
            destination.analysisGain = authority.analysisGain;
        }
        destination.carrier.firstIntegrator =
            source.carrierFirstIntegrator;
        destination.carrier.secondIntegrator =
            source.carrierSecondIntegrator;
        destination.analysis.firstIntegrator =
            source.analysisFirstIntegrator;
        destination.analysis.secondIntegrator =
            source.analysisSecondIntegrator;
        destination.envelope = source.envelope;
    }
    if (mappedConfigurationMode_)
        mappedConfigurationReady_ = state.mappedConfigurationReady;
    return LinearVocoderBankStatus::Ok;
}

void PreparedLinearVocoderBank::reset() noexcept
{
    if (mappedConfigurationMode_)
        resetMappedConfiguration();
    else
        resetState();
}

void PreparedLinearVocoderBank::resetState() noexcept
{
    for (auto& band : bands_)
    {
        band.carrier = {};
        band.analysis = {};
        band.envelope = 0.0;
    }
}

void PreparedLinearVocoderBank::resetMappedConfiguration() noexcept
{
    for (auto& band : bands_)
        band = BandState{};
    mappedConfigurationReady_ = false;
}

const LinearVocoderBankConfiguration&
PreparedLinearVocoderBank::configuration() const noexcept
{
    return configuration_;
}

std::size_t PreparedLinearVocoderBank::bandCount() const noexcept
{
    return bands_.size();
}

double PreparedLinearVocoderBank::envelope(
    std::size_t bandIndex) const noexcept
{
    return bandIndex < bands_.size() ? bands_[bandIndex].envelope : 0.0;
}

double PreparedLinearVocoderBank::carrierCenterFrequency(
    std::size_t bandIndex) const noexcept
{
    return bandIndex < bands_.size()
        ? bands_[bandIndex].carrierCenterFrequency : 0.0;
}

double PreparedLinearVocoderBank::analysisCenterFrequency(
    std::size_t bandIndex) const noexcept
{
    return bandIndex < bands_.size()
        ? bands_[bandIndex].analysisCenterFrequency : 0.0;
}
}
