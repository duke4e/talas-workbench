#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_crossfade
{
enum class Law
{
    Power,
    Linear,
};

enum class Error
{
    None,
    SizeMismatch,
    InvalidLaw,
    FirstInputNonFinite,
    SecondInputNonFinite,
    PanInvalid,
    ScaleInvalid,
    FadeInvalid,
    FadeGainNonFinite,
    FirstFadeProductNonFinite,
    SecondFadeProductNonFinite,
    SumNonFinite,
    ScaledMixNonFinite,
    PanGainNonFinite,
    LeftProductNonFinite,
    RightProductNonFinite,
};

struct Frame
{
    double left = 0.0;
    double right = 0.0;
};

[[nodiscard]] inline Error advance(
    double firstInput,
    double secondInput,
    double pan,
    double scale,
    double fade,
    Law law,
    Frame& output) noexcept
{
    if (law != Law::Power && law != Law::Linear)
        return Error::InvalidLaw;
    if (!std::isfinite(firstInput))
        return Error::FirstInputNonFinite;
    if (!std::isfinite(secondInput))
        return Error::SecondInputNonFinite;
    if (!std::isfinite(pan) || pan < 0.0 || pan > 1.0)
        return Error::PanInvalid;
    if (!std::isfinite(scale) || scale < 0.0)
        return Error::ScaleInvalid;
    if (!std::isfinite(fade) || fade < 0.0 || fade > 1.0)
        return Error::FadeInvalid;

    constexpr double halfPi = 1.57079632679489661923;
    const auto firstGain = law == Law::Linear
        ? 1.0 - fade
        : std::cos(fade * halfPi);
    const auto secondGain = law == Law::Linear
        ? fade
        : std::sin(fade * halfPi);
    if (!std::isfinite(firstGain) || !std::isfinite(secondGain))
        return Error::FadeGainNonFinite;
    const auto firstProduct = firstInput * firstGain;
    if (!std::isfinite(firstProduct))
        return Error::FirstFadeProductNonFinite;
    const auto secondProduct = secondInput * secondGain;
    if (!std::isfinite(secondProduct))
        return Error::SecondFadeProductNonFinite;
    const auto sum = firstProduct + secondProduct;
    if (!std::isfinite(sum))
        return Error::SumNonFinite;
    const auto scaledMix = sum * scale;
    if (!std::isfinite(scaledMix))
        return Error::ScaledMixNonFinite;
    const auto leftGain = std::cos(pan * halfPi);
    const auto rightGain = std::sin(pan * halfPi);
    if (!std::isfinite(leftGain) || !std::isfinite(rightGain))
        return Error::PanGainNonFinite;
    const auto left = scaledMix * leftGain;
    if (!std::isfinite(left))
        return Error::LeftProductNonFinite;
    const auto right = scaledMix * rightGain;
    if (!std::isfinite(right))
        return Error::RightProductNonFinite;
    output = {left, right};
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> firstInput,
    std::span<const double> secondInput,
    std::optional<std::span<const double>> connectedPan,
    double staticPan,
    std::optional<std::span<const double>> connectedScale,
    double staticScale,
    std::optional<std::span<const double>> connectedFade,
    double staticFade,
    Law law,
    std::span<double> leftCandidate,
    std::span<double> rightCandidate,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    const auto frameCount = firstInput.size();
    if (secondInput.size() != frameCount
        || leftCandidate.size() != frameCount
        || rightCandidate.size() != frameCount
        || leftOutput.size() != frameCount
        || rightOutput.size() != frameCount
        || (connectedPan.has_value()
            && connectedPan->size() != frameCount)
        || (connectedScale.has_value()
            && connectedScale->size() != frameCount)
        || (connectedFade.has_value()
            && connectedFade->size() != frameCount))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        Frame candidate;
        const auto error = advance(
            firstInput[frame],
            secondInput[frame],
            connectedPan.has_value()
                ? (*connectedPan)[frame]
                : staticPan,
            connectedScale.has_value()
                ? (*connectedScale)[frame]
                : staticScale,
            connectedFade.has_value()
                ? (*connectedFade)[frame]
                : staticFade,
            law,
            candidate);
        if (error != Error::None)
            return error;
        leftCandidate[frame] = candidate.left;
        rightCandidate[frame] = candidate.right;
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        leftOutput[frame] = leftCandidate[frame];
        rightOutput[frame] = rightCandidate[frame];
    }
    return Error::None;
}
}
