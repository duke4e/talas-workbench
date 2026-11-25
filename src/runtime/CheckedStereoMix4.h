#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_stereo_mix4
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
    ThirdInputNonFinite,
    ThirdPanInvalid,
    ThirdScaleInvalid,
    ThirdScaledSampleNonFinite,
    ThirdPanGainNonFinite,
    ThirdLeftContributionNonFinite,
    ThirdRightContributionNonFinite,
    ThirdLeftAccumulationNonFinite,
    ThirdRightAccumulationNonFinite,
    FourthInputNonFinite,
    FourthPanInvalid,
    FourthScaleInvalid,
    FourthScaledSampleNonFinite,
    FourthPanGainNonFinite,
    FourthLeftContributionNonFinite,
    FourthRightContributionNonFinite,
    FourthLeftAccumulationNonFinite,
    FourthRightAccumulationNonFinite,
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
    Control pan3;
    Control scale3;
    Control pan4;
    Control scale4;
};

[[nodiscard]] inline Error advance(
    double firstInput,
    double secondInput,
    double thirdInput,
    double fourthInput,
    double leftLevel,
    double rightLevel,
    double firstPan,
    double firstScale,
    double secondPan,
    double secondScale,
    double thirdPan,
    double thirdScale,
    double fourthPan,
    double fourthScale,
    Frame& output) noexcept
{
    if (!std::isfinite(leftLevel) || leftLevel < -1.0 || leftLevel > 1.0)
        return Error::LeftLevelInvalid;
    if (!std::isfinite(rightLevel) || rightLevel < -1.0 || rightLevel > 1.0)
        return Error::RightLevelInvalid;

    constexpr double halfPi = 1.57079632679489661923;
    double leftMix = 0.0;
    double rightMix = 0.0;
    const auto accumulate = [&] (
        double input,
        double pan,
        double scale,
        Error inputError,
        Error panError,
        Error scaleError,
        Error scaledError,
        Error gainError,
        Error leftContributionError,
        Error rightContributionError,
        Error leftAccumulationError,
        Error rightAccumulationError) noexcept {
        if (!std::isfinite(input))
            return inputError;
        if (!std::isfinite(pan) || pan < 0.0 || pan > 1.0)
            return panError;
        if (!std::isfinite(scale) || scale < 0.0 || scale > 1.0)
            return scaleError;
        const auto scaled = input * scale;
        if (!std::isfinite(scaled))
            return scaledError;
        const auto leftGain = std::cos(pan * halfPi);
        const auto rightGain = std::sin(pan * halfPi);
        if (!std::isfinite(leftGain) || !std::isfinite(rightGain))
            return gainError;
        const auto leftContribution = scaled * leftGain;
        if (!std::isfinite(leftContribution))
            return leftContributionError;
        const auto rightContribution = scaled * rightGain;
        if (!std::isfinite(rightContribution))
            return rightContributionError;
        leftMix += leftContribution;
        if (!std::isfinite(leftMix))
            return leftAccumulationError;
        rightMix += rightContribution;
        if (!std::isfinite(rightMix))
            return rightAccumulationError;
        return Error::None;
    };

    auto error = accumulate(
        firstInput, firstPan, firstScale,
        Error::FirstInputNonFinite, Error::FirstPanInvalid,
        Error::FirstScaleInvalid, Error::FirstScaledSampleNonFinite,
        Error::FirstPanGainNonFinite,
        Error::FirstLeftContributionNonFinite,
        Error::FirstRightContributionNonFinite,
        Error::FirstLeftAccumulationNonFinite,
        Error::FirstRightAccumulationNonFinite);
    if (error != Error::None)
        return error;
    error = accumulate(
        secondInput, secondPan, secondScale,
        Error::SecondInputNonFinite, Error::SecondPanInvalid,
        Error::SecondScaleInvalid, Error::SecondScaledSampleNonFinite,
        Error::SecondPanGainNonFinite,
        Error::SecondLeftContributionNonFinite,
        Error::SecondRightContributionNonFinite,
        Error::SecondLeftAccumulationNonFinite,
        Error::SecondRightAccumulationNonFinite);
    if (error != Error::None)
        return error;
    error = accumulate(
        thirdInput, thirdPan, thirdScale,
        Error::ThirdInputNonFinite, Error::ThirdPanInvalid,
        Error::ThirdScaleInvalid, Error::ThirdScaledSampleNonFinite,
        Error::ThirdPanGainNonFinite,
        Error::ThirdLeftContributionNonFinite,
        Error::ThirdRightContributionNonFinite,
        Error::ThirdLeftAccumulationNonFinite,
        Error::ThirdRightAccumulationNonFinite);
    if (error != Error::None)
        return error;
    error = accumulate(
        fourthInput, fourthPan, fourthScale,
        Error::FourthInputNonFinite, Error::FourthPanInvalid,
        Error::FourthScaleInvalid, Error::FourthScaledSampleNonFinite,
        Error::FourthPanGainNonFinite,
        Error::FourthLeftContributionNonFinite,
        Error::FourthRightContributionNonFinite,
        Error::FourthLeftAccumulationNonFinite,
        Error::FourthRightAccumulationNonFinite);
    if (error != Error::None)
        return error;

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
    std::span<const double> thirdInput,
    std::span<const double> fourthInput,
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
        || thirdInput.size() != frameCount
        || fourthInput.size() != frameCount
        || leftCandidate.size() != frameCount
        || rightCandidate.size() != frameCount
        || leftOutput.size() != frameCount
        || rightOutput.size() != frameCount
        || controlHasWrongSize(controls.left)
        || controlHasWrongSize(controls.right)
        || controlHasWrongSize(controls.pan1)
        || controlHasWrongSize(controls.scale1)
        || controlHasWrongSize(controls.pan2)
        || controlHasWrongSize(controls.scale2)
        || controlHasWrongSize(controls.pan3)
        || controlHasWrongSize(controls.scale3)
        || controlHasWrongSize(controls.pan4)
        || controlHasWrongSize(controls.scale4))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        Frame candidate;
        const auto error = advance(
            firstInput[frame], secondInput[frame],
            thirdInput[frame], fourthInput[frame],
            controls.left.value(frame), controls.right.value(frame),
            controls.pan1.value(frame), controls.scale1.value(frame),
            controls.pan2.value(frame), controls.scale2.value(frame),
            controls.pan3.value(frame), controls.scale3.value(frame),
            controls.pan4.value(frame), controls.scale4.value(frame),
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
