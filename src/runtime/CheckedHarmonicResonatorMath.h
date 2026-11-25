#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace talas::runtime::checked_harmonic_resonator
{
struct Period
{
    double seconds = 0.0;
    std::size_t delaySamples = 0;

    bool operator==(const Period&) const = default;
};

struct FrameCandidate
{
    double output = 0.0;
    double writeSample = 0.0;
    double brightnessLowPass = 0.0;

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

[[nodiscard]] inline bool preparePeriod(
    double sampleRate,
    double frequency,
    std::size_t maximumDelaySamples,
    Period& result) noexcept
{
    if (!std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || !std::isfinite(frequency)
        || frequency <= 0.0)
    {
        return false;
    }

    const auto periodSeconds = 1.0 / frequency;
    if (!std::isfinite(periodSeconds) || periodSeconds <= 0.0)
        return false;

    const auto delaySamples = static_cast<long double>(periodSeconds)
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

    result = {
        periodSeconds,
        static_cast<std::size_t>(roundedDelaySamples),
    };
    return true;
}

[[nodiscard]] inline bool computeFeedback(
    double periodSeconds,
    double decayTime,
    double& result) noexcept
{
    if (!std::isfinite(periodSeconds)
        || periodSeconds <= 0.0
        || !std::isfinite(decayTime)
        || decayTime <= 0.0)
    {
        return false;
    }

    double ratio = 0.0;
    if (!checkedDivide(periodSeconds, decayTime, ratio)
        || ratio < 0.0)
    {
        return false;
    }
    const auto feedback = std::pow(0.001, ratio);
    if (!std::isfinite(feedback)
        || feedback < 0.0
        || feedback >= 1.0)
    {
        return false;
    }
    result = feedback;
    return true;
}

// The resonances fall at the Frequency and its harmonics. The
// delay line is read between two samples through a first-order allpass
// whose delay at the Frequency makes up the fraction, and the Brightness
// filter's own delay there is taken off the loop.
struct Tuning
{
    // The allpass reads nearerDelay and nearerDelay + 1 samples back.
    std::size_t nearerDelay = 0;
    double allpassCoefficient = 0.0;
    bool fractional = false;
};

[[nodiscard]] inline Tuning tune(
    double sampleRate,
    double frequency,
    std::size_t roundedDelay,
    double brightness,
    double feedback) noexcept
{
    constexpr double pi = 3.1415926535897932384626433832795;
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(frequency) || frequency <= 0.0
        || 2.0 * frequency >= sampleRate)
    {
        return {};
    }
    const auto omega = 2.0 * pi * frequency / sampleRate;
    auto loopDelay = sampleRate / frequency;
    if (brightness != 1.0 && std::isfinite(brightness) && brightness >= 0.0)
    {
        // The shaped feedback is g times the delayed sample plus 1 - g
        // times its one-pole low pass, which delays it by -phase / omega.
        auto gain = brightness;
        if (std::abs(feedback) > 0.0)
            gain = std::min(brightness, 0.999 / std::abs(feedback));
        const auto cosine = std::cos(omega);
        const auto sine = std::sin(omega);
        const auto phase =
            std::atan2(0.5 * gain * sine, 0.5 + 0.5 * gain - 0.5 * gain * cosine)
            - std::atan2(0.5 * sine, 1.0 - 0.5 * cosine);
        loopDelay += phase / omega;
    }
    // The further read, nearerDelay + 1, is loopDelay rounded, which the
    // delay line holds as long as it stays within the rounded period.
    loopDelay = std::min(loopDelay, static_cast<double>(roundedDelay) + 0.5 - 1.0e-9);
    if (!std::isfinite(loopDelay) || loopDelay < 1.5)
        return {};
    auto nearer = std::floor(loopDelay);
    auto fraction = loopDelay - nearer;
    if (fraction < 0.5)
    {
        nearer -= 1.0;
        fraction += 1.0;
    }
    // The coefficient whose phase delay at the Frequency is the fraction,
    // or, where that is unsteady, the one whose delay is the fraction at
    // low frequencies.
    auto coefficient = std::sin(0.5 * (1.0 - fraction) * omega)
        / std::sin(0.5 * (1.0 + fraction) * omega);
    if (!std::isfinite(coefficient) || std::abs(coefficient) > 0.5)
        coefficient = (1.0 - fraction) / (1.0 + fraction);
    return {static_cast<std::size_t>(nearer), coefficient, true};
}

// The allpass's output from the samples nearerDelay and nearerDelay + 1
// back and its previous output.
[[nodiscard]] inline double allpassRead(
    double coefficient, double nearer, double further, double previous) noexcept
{
    return coefficient * (nearer - previous) + further;
}

[[nodiscard]] inline bool processFrame(
    double input,
    double scale,
    double brightness,
    double feedback,
    std::size_t delaySamples,
    double delayed,
    double currentBrightnessLowPass,
    FrameCandidate& result) noexcept
{
    if (!std::isfinite(input)
        || !std::isfinite(scale)
        || !std::isfinite(brightness)
        || brightness < 0.0)
    {
        return false;
    }

    double excitation = 0.0;
    if (!checkedMultiply(input, scale, excitation))
        return false;

    if (delaySamples == 0)
    {
        result = {excitation, excitation, 0.0};
        return true;
    }

    if (!std::isfinite(delayed)
        || !std::isfinite(feedback)
        || feedback < 0.0
        || feedback >= 1.0
        || !std::isfinite(currentBrightnessLowPass))
    {
        return false;
    }

    auto brightnessLowPass = currentBrightnessLowPass;
    auto shapedDelayed = delayed;
    if (brightness != 1.0)
    {
        double lowDifference = 0.0;
        double lowIncrement = 0.0;
        double high = 0.0;
        if (!checkedSubtract(
                delayed,
                brightnessLowPass,
                lowDifference)
            || !checkedMultiply(0.5, lowDifference, lowIncrement)
            || !checkedAdd(
                brightnessLowPass,
                lowIncrement,
                brightnessLowPass)
            || !checkedSubtract(delayed, brightnessLowPass, high))
        {
            return false;
        }

        const auto feedbackMagnitude = std::abs(feedback);
        auto stableHighGain = brightness;
        if (feedbackMagnitude > 0.0)
        {
            double maximumStableGain = 0.0;
            if (!checkedDivide(
                    0.999,
                    feedbackMagnitude,
                    maximumStableGain))
            {
                return false;
            }
            stableHighGain = std::min(
                brightness,
                maximumStableGain);
        }

        double shapedHigh = 0.0;
        if (!checkedMultiply(high, stableHighGain, shapedHigh)
            || !checkedAdd(
                brightnessLowPass,
                shapedHigh,
                shapedDelayed))
        {
            return false;
        }
    }

    double feedbackSample = 0.0;
    double writeSample = 0.0;
    if (!checkedMultiply(shapedDelayed, feedback, feedbackSample)
        || !checkedAdd(excitation, feedbackSample, writeSample))
    {
        return false;
    }

    result = {delayed, writeSample, brightnessLowPass};
    return true;
}
}
