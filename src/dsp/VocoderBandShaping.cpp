#include "talas/dsp/VocoderBandShaping.h"

#include <algorithm>
#include <cmath>

namespace talas::dsp
{
VocoderBandShapingResult deriveVocoderBandShaping(
    std::span<const double> carrierCenterFrequencies,
    double tone,
    double rolloff) noexcept
{
    return deriveVocoderBandShaping(carrierCenterFrequencies,
        VocoderBandShaping{VocoderBandShapingMode::LegacyTilt, tone, rolloff});
}

namespace
{
// A Butterworth-like edge: 1 well inside, falling a factor 1 / sqrt(2) at
// the cutoff, and 0 where the power overflows far outside.
[[nodiscard]] double edgeGain(double ratio, double order) noexcept
{
    const auto power = std::pow(ratio, 2.0 * order);
    if (!std::isfinite(power))
        return 0.0;
    return 1.0 / std::sqrt(1.0 + power);
}
}

VocoderBandShapingResult deriveVocoderBandShaping(
    std::span<const double> carrierCenterFrequencies,
    const VocoderBandShaping& shaping) noexcept
{
    const auto tone = shaping.tone;
    const auto rolloff = shaping.rolloff;
    VocoderBandShapingResult result;
    if (carrierCenterFrequencies.empty()
        || carrierCenterFrequencies.size() > maximumLinearVocoderBandCount)
    {
        return result;
    }
    result.count = carrierCenterFrequencies.size();
    if (!std::isfinite(tone) || tone < -1.0 || tone > 1.0)
    {
        result.status = VocoderBandShapingStatus::InvalidTone;
        return result;
    }
    if (!std::isfinite(rolloff) || rolloff < 0.0 || rolloff > 4.0)
    {
        result.status = VocoderBandShapingStatus::InvalidRolloff;
        return result;
    }
    for (std::size_t index = 0;
         index < carrierCenterFrequencies.size(); ++index)
    {
        const auto center = carrierCenterFrequencies[index];
        if (!std::isfinite(center) || center <= 0.0
            || (index != 0
                && center <= carrierCenterFrequencies[index - 1]))
        {
            result.status =
                VocoderBandShapingStatus::InvalidCenterFrequency;
            return result;
        }
    }

    if (shaping.mode == VocoderBandShapingMode::Cutoffs)
    {
        if (!std::isfinite(shaping.loCutoff) || shaping.loCutoff < 0.0
            || !std::isfinite(shaping.hiCutoff) || shaping.hiCutoff <= 0.0
            || shaping.loCutoff > shaping.hiCutoff)
        {
            result.status = VocoderBandShapingStatus::InvalidCenterFrequency;
            return result;
        }
        // Rolloff 1 is precipitous; 0.5 falls 6 dB an octave.
        const auto steepness = std::clamp(rolloff, 0.0, 1.0);
        const auto order = steepness >= 1.0
            ? 64.0 : std::min(64.0, steepness / (1.0 - steepness));
        for (std::size_t index = 0;
             index < carrierCenterFrequencies.size(); ++index)
        {
            const auto center = carrierCenterFrequencies[index];
            const auto low = shaping.loCutoff > 0.0
                ? edgeGain(shaping.loCutoff / center, order) : 1.0;
            const auto gain = low * edgeGain(center / shaping.hiCutoff, order);
            if (!std::isfinite(gain) || gain < 0.0 || gain > 1.0)
            {
                result.status = VocoderBandShapingStatus::NumericOverflow;
                return result;
            }
            result.carrierGains[index] = gain;
        }
        result.status = VocoderBandShapingStatus::Ok;
        return result;
    }

    const auto lowest = carrierCenterFrequencies.front();
    const auto highest = carrierCenterFrequencies.back();
    const auto logSpan = carrierCenterFrequencies.size() == 1
        ? 0.0 : std::log2(highest / lowest);
    if (!std::isfinite(logSpan) || logSpan < 0.0)
    {
        result.status = VocoderBandShapingStatus::NumericOverflow;
        return result;
    }
    for (std::size_t index = 0;
         index < carrierCenterFrequencies.size(); ++index)
    {
        const auto position = carrierCenterFrequencies.size() == 1
            ? 0.5
            : std::clamp(
                std::log2(carrierCenterFrequencies[index] / lowest)
                    / logSpan,
                0.0,
                1.0);
        // A TunableVocoder's tilt reaches as far as Rolloff, the loudest
        // filter at 1.
        const auto exponent = shaping.mode == VocoderBandShapingMode::ToneWidth
            ? rolloff * (tone * (2.0 * position - 1.0) - std::abs(tone))
            : tone * (2.0 * position - 1.0) - rolloff * position;
        const auto gain = std::exp2(exponent);
        const auto lowestGain = shaping.mode == VocoderBandShapingMode::ToneWidth
            ? 0.00390625 : 0.03125;
        if (!std::isfinite(position) || !std::isfinite(exponent)
            || !std::isfinite(gain) || gain < lowestGain || gain > 2.0)
        {
            result.status = VocoderBandShapingStatus::NumericOverflow;
            return result;
        }
        result.carrierGains[index] = gain;
    }
    result.status = VocoderBandShapingStatus::Ok;
    return result;
}

LinearVocoderBankStatus configurePreparedVocoderBandShaping(
    PreparedLinearVocoderBank& bank,
    const VocoderBandShaping& shaping) noexcept
{
    std::array<double, maximumLinearVocoderBandCount> centers{};
    for (std::size_t index = 0; index < bank.bandCount(); ++index)
        centers[index] = bank.carrierCenterFrequency(index);
    const auto shaped = deriveVocoderBandShaping(
        std::span{centers}.first(bank.bandCount()), shaping);
    if (shaped.status != VocoderBandShapingStatus::Ok)
        return LinearVocoderBankStatus::InvalidConfiguration;
    return bank.configurePreparedCarrierGains(shaped.gains());
}

LinearVocoderBankStatus configurePreparedVocoderBandShaping(
    PreparedLinearVocoderBank& bank,
    double tone,
    double rolloff) noexcept
{
    return configurePreparedVocoderBandShaping(bank,
        VocoderBandShaping{VocoderBandShapingMode::LegacyTilt, tone, rolloff});
}

LinearVocoderBankStatus applyVocoderTuning(
    PreparedLinearVocoderBank& bank,
    VocoderTuning& last,
    const VocoderTuning& next) noexcept
{
    if (next == last)
        return LinearVocoderBankStatus::Ok;
    const auto count = std::min(next.configuration.bandCount, next.centers.size());
    if (!std::isfinite(next.tone) || next.tone < -1.0 || next.tone > 1.0
        || !std::isfinite(next.rolloff) || next.rolloff < 0.0 || next.rolloff > 4.0)
    {
        return LinearVocoderBankStatus::InvalidConfiguration;
    }
    const auto centers = std::span<const double>{next.centers}.first(
        next.explicitCenters ? count : 0);
    const auto status = bank.retune(next.configuration, centers, centers);
    if (status != LinearVocoderBankStatus::Ok)
        return status;
    // A Tone and Rolloff in range always shape centers the bank took.
    const auto shaped = configurePreparedVocoderBandShaping(bank,
        VocoderBandShaping{next.shaping, next.tone, next.rolloff, next.loCutoff, next.hiCutoff});
    if (shaped != LinearVocoderBankStatus::Ok)
        return shaped;
    last = next;
    return LinearVocoderBankStatus::Ok;
}
}
