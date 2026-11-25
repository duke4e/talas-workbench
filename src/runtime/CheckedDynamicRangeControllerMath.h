#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <string_view>

namespace talas::runtime::checked_dynamic_range_controller
{

enum class Mode
{
    Compressor,
    Expander,
    // The Input boosted while the SideChain is
    // below the Threshold, by the decibels the Expander would cut.
    UpwardExpander,
};

// The Type a DynamicRangeController's choice names, or none.
[[nodiscard]] inline std::optional<Mode> modeNamed(std::string_view type) noexcept
{
    if (type == "compressor")
        return Mode::Compressor;
    if (type == "expander")
        return Mode::Expander;
    if (type == "upwardExpander")
        return Mode::UpwardExpander;
    return std::nullopt;
}

// The smallest envelope an UpwardExpander boosts from, a 24-bit step of
// full scale, so that a silent SideChain gives a finite boost.
inline constexpr double upwardExpanderEnvelopeFloor = 1.0 / 16777216.0;

struct FrameCandidate
{
    double envelope = 0.0;
    double output = 0.0;

    bool operator==(const FrameCandidate&) const = default;
};

[[nodiscard]] inline bool checkedMultiply(
    double left,
    double right,
    double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left * right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedAdd(
    double left,
    double right,
    double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left + right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedSubtract(
    double left,
    double right,
    double& result) noexcept
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return false;
    const auto candidate = left - right;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool checkedDivide(
    double numerator,
    double denominator,
    double& result) noexcept
{
    if (!std::isfinite(numerator)
        || !std::isfinite(denominator)
        || denominator == 0.0)
    {
        return false;
    }
    const auto candidate = numerator / denominator;
    if (!std::isfinite(candidate))
        return false;
    result = candidate;
    return true;
}

[[nodiscard]] inline bool validateRatio(
    Mode mode,
    double ratio) noexcept
{
    return std::isfinite(ratio)
        && ratio > 0.0
        && (mode == Mode::Compressor ? ratio >= 1.0 : ratio <= 1.0);
}

// A live Ratio past what the Type takes saturates at its edge (owner,
// 2026-10-12; above 1 for compression, below it
// for expansion): 1 for either, a fixed-point step above zero for an
// expander. A value that is not finite is left to be refused.
[[nodiscard]] inline double saturateRatio(
    Mode mode,
    double ratio) noexcept
{
    if (!std::isfinite(ratio))
        return ratio;
    return mode == Mode::Compressor
        ? std::max(ratio, 1.0)
        : std::clamp(ratio, 2.0 / 16777216.0, 1.0);
}

[[nodiscard]] inline bool prepareDelaySamples(
    double sampleRate,
    double delaySeconds,
    std::size_t maximumDelaySamples,
    std::size_t& result) noexcept
{
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(delaySeconds)
        || delaySeconds < 0.0)
    {
        return false;
    }
    const auto delaySamples = static_cast<long double>(delaySeconds)
        * static_cast<long double>(sampleRate);
    const auto roundedDelaySamples = std::round(delaySamples);
    constexpr auto maximumSize = static_cast<long double>(
        std::numeric_limits<std::size_t>::max());
    if (!std::isfinite(delaySamples)
        || !std::isfinite(roundedDelaySamples)
        || roundedDelaySamples < 0.0L
        || roundedDelaySamples >= maximumSize
        || roundedDelaySamples
            > static_cast<long double>(maximumDelaySamples))
    {
        return false;
    }
    result = static_cast<std::size_t>(roundedDelaySamples);
    return true;
}

[[nodiscard]] inline bool computeTimeAlpha(
    double sampleRate,
    double timeSeconds,
    double& result) noexcept
{
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(timeSeconds)
        || timeSeconds <= 0.0)
    {
        return false;
    }
    double framesToSixtyPercent = 0.0;
    double exponent = 0.0;
    if (!checkedMultiply(
            timeSeconds,
            sampleRate,
            framesToSixtyPercent)
        || framesToSixtyPercent <= 0.0
        || !checkedDivide(1.0, framesToSixtyPercent, exponent))
    {
        return false;
    }
    const auto alpha = std::pow(0.4, exponent);
    if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0)
        return false;
    result = alpha;
    return true;
}

[[nodiscard]] inline bool processFrame(
    Mode mode,
    double sideChain,
    double delayedInput,
    double ratio,
    double threshold,
    double attackAlpha,
    double releaseAlpha,
    double outputGain,
    double priorEnvelope,
    FrameCandidate& result,
    bool decibelRatio = false) noexcept
{
    if (!std::isfinite(sideChain)
        || !std::isfinite(delayedInput)
        || !validateRatio(mode, ratio)
        || !std::isfinite(threshold)
        || threshold < 0.0
        || !std::isfinite(attackAlpha)
        || attackAlpha < 0.0
        || attackAlpha > 1.0
        || !std::isfinite(releaseAlpha)
        || releaseAlpha < 0.0
        || releaseAlpha > 1.0
        || !std::isfinite(outputGain)
        || !std::isfinite(priorEnvelope)
        || priorEnvelope < 0.0)
    {
        return false;
    }

    const auto targetEnvelope = std::abs(sideChain);
    if (!std::isfinite(targetEnvelope))
        return false;
    const auto alpha = targetEnvelope > priorEnvelope
        ? attackAlpha
        : releaseAlpha;
    double oneMinusAlpha = 0.0;
    double targetContribution = 0.0;
    double priorContribution = 0.0;
    double envelope = 0.0;
    if (!checkedSubtract(1.0, alpha, oneMinusAlpha)
        || !checkedMultiply(
            targetEnvelope,
            oneMinusAlpha,
            targetContribution)
        || !checkedMultiply(
            priorEnvelope,
            alpha,
            priorContribution)
        || !checkedAdd(
            targetContribution,
            priorContribution,
            envelope)
        || envelope < 0.0)
    {
        return false;
    }

    auto dynamicGain = 1.0;
    // An UpwardExpander boosts the Input while
    // the SideChain is below the Threshold. Below it the level in dB moves
    // 1/Ratio as fast as the SideChain's, as an Expander's does, but up
    // instead of down, the boost never raising the SideChain's level past
    // the Threshold; the linear law (projects before format 100) mirrors
    // the Expander's in the same way.
    if (mode == Mode::UpwardExpander)
    {
        if (envelope < threshold && threshold > 0.0)
        {
            const auto floored = std::max(envelope, upwardExpanderEnvelopeFloor);
            const auto ceiling = threshold / floored;
            const auto boost = decibelRatio
                ? std::pow(ceiling, 1.0 / ratio - 1.0)
                : (threshold - (threshold - floored) * ratio) / floored;
            if (!std::isfinite(ceiling))
                return false;
            dynamicGain = std::isfinite(boost)
                ? std::clamp(boost, 1.0, std::max(1.0, ceiling))
                : std::max(1.0, ceiling);
        }
    }
    // A Ratio in decibels: past the Threshold the
    // level in dB changes 1/Ratio as fast as the SideChain's, so the
    // output level is Threshold * (envelope / Threshold)^(1/Ratio).
    if (decibelRatio
        && ((mode == Mode::Compressor && envelope > threshold && envelope > 0.0)
            || (mode == Mode::Expander && envelope < threshold && threshold > 0.0)))
    {
        if (envelope <= 0.0)
        {
            dynamicGain = 0.0;
        }
        else
        {
            const auto exponent = 1.0 / ratio - 1.0;
            dynamicGain = std::pow(envelope / threshold, exponent);
            if (!std::isfinite(exponent) || !std::isfinite(dynamicGain))
                return false;
        }
    }
    else if (mode == Mode::Compressor
        && envelope > threshold
        && envelope > 0.0)
    {
        double levelAboveThreshold = 0.0;
        double controlledExcess = 0.0;
        double controlledLevel = 0.0;
        if (!checkedSubtract(
                envelope,
                threshold,
                levelAboveThreshold)
            || !checkedDivide(
                levelAboveThreshold,
                ratio,
                controlledExcess)
            || !checkedAdd(
                threshold,
                controlledExcess,
                controlledLevel)
            || !checkedDivide(
                controlledLevel,
                envelope,
                dynamicGain))
        {
            return false;
        }
    }
    else if (mode == Mode::Expander
             && envelope < threshold
             && threshold > 0.0)
    {
        if (envelope <= 0.0)
        {
            dynamicGain = 0.0;
        }
        else
        {
            double levelBelowThreshold = 0.0;
            double expandedDifference = 0.0;
            double controlledLevel = 0.0;
            if (!checkedSubtract(
                    threshold,
                    envelope,
                    levelBelowThreshold)
                || !checkedDivide(
                    levelBelowThreshold,
                    ratio,
                    expandedDifference)
                || !checkedSubtract(
                    threshold,
                    expandedDifference,
                    controlledLevel))
            {
                return false;
            }
            controlledLevel = std::max(0.0, controlledLevel);
            if (!checkedDivide(
                    controlledLevel,
                    envelope,
                    dynamicGain))
            {
                return false;
            }
        }
    }

    double controlledInput = 0.0;
    double output = 0.0;
    if (!checkedMultiply(
            delayedInput,
            dynamicGain,
            controlledInput)
        || !checkedMultiply(
            controlledInput,
            outputGain,
            output))
    {
        return false;
    }

    result = {envelope, output};
    return true;
}

}
