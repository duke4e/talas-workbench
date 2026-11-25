#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_stereo_mix2
{
enum class Error
{
    None,
    SizeMismatch,
    LeftLevelInvalid,
    RightLevelInvalid,
    FirstInputNonFinite,
    FirstPanInvalid,
    FirstScaleInvalid,
    FirstScaledSampleNonFinite,
    FirstPanGainNonFinite,
    FirstLeftContributionNonFinite,
    FirstRightContributionNonFinite,
    FirstLeftAccumulationNonFinite,
    FirstRightAccumulationNonFinite,
    SecondInputNonFinite,
    SecondPanInvalid,
    SecondScaleInvalid,
    SecondScaledSampleNonFinite,
    SecondPanGainNonFinite,
    SecondLeftContributionNonFinite,
    SecondRightContributionNonFinite,
    SecondLeftAccumulationNonFinite,
    SecondRightAccumulationNonFinite,
    LeftOutputProductNonFinite,
    RightOutputProductNonFinite,
};

struct Frame
{
    double left = 0.0;
    double right = 0.0;
};

struct Control
{
    std::optional<std::span<const double>> connected;
    double fallback = 0.0;

    [[nodiscard]] double value(std::size_t frame) const noexcept
    {
        return connected.has_value() ? (*connected)[frame] : fallback;
    }
};

struct Controls
{
    Control left;
    Control right;
    Control pan1;
    Control scale1;
    Control pan2;
    Control scale2;
};

[[nodiscard]] inline Error advance(
    double firstInput,
    double secondInput,
    double leftLevel,
    double rightLevel,
    double firstPan,
    double firstScale,
    double secondPan,
    double secondScale,
    Frame& output) noexcept
{
    if (!std::isfinite(leftLevel) || leftLevel < -1.0 || leftLevel > 1.0)
        return Error::LeftLevelInvalid;
    if (!std::isfinite(rightLevel) || rightLevel < -1.0 || rightLevel > 1.0)
        return Error::RightLevelInvalid;

    constexpr double halfPi = 1.57079632679489661923;
    double leftMix = 0.0;
    double rightMix = 0.0;

    if (!std::isfinite(firstInput))
        return Error::FirstInputNonFinite;
    if (!std::isfinite(firstPan) || firstPan < 0.0 || firstPan > 1.0)
        return Error::FirstPanInvalid;
    if (!std::isfinite(firstScale) || firstScale < 0.0 || firstScale > 1.0)
        return Error::FirstScaleInvalid;
    const auto firstScaled = firstInput * firstScale;
    if (!std::isfinite(firstScaled))
        return Error::FirstScaledSampleNonFinite;
    const auto firstLeftGain = std::cos(firstPan * halfPi);
    const auto firstRightGain = std::sin(firstPan * halfPi);
    if (!std::isfinite(firstLeftGain) || !std::isfinite(firstRightGain))
        return Error::FirstPanGainNonFinite;
    const auto firstLeftContribution = firstScaled * firstLeftGain;
    if (!std::isfinite(firstLeftContribution))
        return Error::FirstLeftContributionNonFinite;
    const auto firstRightContribution = firstScaled * firstRightGain;
    if (!std::isfinite(firstRightContribution))
        return Error::FirstRightContributionNonFinite;
    leftMix += firstLeftContribution;
    if (!std::isfinite(leftMix))
        return Error::FirstLeftAccumulationNonFinite;
    rightMix += firstRightContribution;
    if (!std::isfinite(rightMix))
        return Error::FirstRightAccumulationNonFinite;

    if (!std::isfinite(secondInput))
        return Error::SecondInputNonFinite;
    if (!std::isfinite(secondPan) || secondPan < 0.0 || secondPan > 1.0)
        return Error::SecondPanInvalid;
    if (!std::isfinite(secondScale) || secondScale < 0.0 || secondScale > 1.0)
        return Error::SecondScaleInvalid;
    const auto secondScaled = secondInput * secondScale;
    if (!std::isfinite(secondScaled))
        return Error::SecondScaledSampleNonFinite;
    const auto secondLeftGain = std::cos(secondPan * halfPi);
    const auto secondRightGain = std::sin(secondPan * halfPi);
    if (!std::isfinite(secondLeftGain) || !std::isfinite(secondRightGain))
        return Error::SecondPanGainNonFinite;
    const auto secondLeftContribution = secondScaled * secondLeftGain;
    if (!std::isfinite(secondLeftContribution))
        return Error::SecondLeftContributionNonFinite;
    const auto secondRightContribution = secondScaled * secondRightGain;
    if (!std::isfinite(secondRightContribution))
        return Error::SecondRightContributionNonFinite;
    leftMix += secondLeftContribution;
    if (!std::isfinite(leftMix))
        return Error::SecondLeftAccumulationNonFinite;
    rightMix += secondRightContribution;
    if (!std::isfinite(rightMix))
        return Error::SecondRightAccumulationNonFinite;

    const auto left = leftMix * leftLevel;
    if (!std::isfinite(left))
        return Error::LeftOutputProductNonFinite;
    const auto right = rightMix * rightLevel;
    if (!std::isfinite(right))
        return Error::RightOutputProductNonFinite;
    output = {left, right};
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> firstInput,
    std::span<const double> secondInput,
    const Controls& controls,
    std::span<double> leftCandidate,
    std::span<double> rightCandidate,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    const auto frameCount = firstInput.size();
    const auto controlHasWrongSize = [frameCount](const Control& control) {
        return control.connected.has_value()
            && control.connected->size() != frameCount;
    };
    if (secondInput.size() != frameCount
        || leftCandidate.size() != frameCount
        || rightCandidate.size() != frameCount
        || leftOutput.size() != frameCount
        || rightOutput.size() != frameCount
        || controlHasWrongSize(controls.left)
        || controlHasWrongSize(controls.right)
        || controlHasWrongSize(controls.pan1)
        || controlHasWrongSize(controls.scale1)
        || controlHasWrongSize(controls.pan2)
        || controlHasWrongSize(controls.scale2))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        Frame candidate;
        const auto error = advance(
            firstInput[frame],
            secondInput[frame],
            controls.left.value(frame),
            controls.right.value(frame),
            controls.pan1.value(frame),
            controls.scale1.value(frame),
            controls.pan2.value(frame),
            controls.scale2.value(frame),
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
