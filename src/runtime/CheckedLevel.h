#pragma once

#include <algorithm>

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_level
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    LeftGainNonFinite,
    RightGainNonFinite,
    LeftProductNonFinite,
    RightProductNonFinite,
};

[[nodiscard]] inline Error advance(
    double input,
    double leftGain,
    double rightGain,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(input))
        return Error::InputNonFinite;
    if (!std::isfinite(leftGain))
        return Error::LeftGainNonFinite;
    if (!std::isfinite(rightGain))
        return Error::RightGainNonFinite;

    const auto candidateLeft = input * leftGain;
    if (!std::isfinite(candidateLeft))
        return Error::LeftProductNonFinite;
    const auto candidateRight = input * rightGain;
    if (!std::isfinite(candidateRight))
        return Error::RightProductNonFinite;

    leftOutput = candidateLeft;
    rightOutput = candidateRight;
    return Error::None;
}

// A stereo input: each channel's own gain on its own sample.
[[nodiscard]] inline Error advance(
    double leftInput,
    double rightInput,
    double leftGain,
    double rightGain,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(leftInput) || !std::isfinite(rightInput))
        return Error::InputNonFinite;
    if (!std::isfinite(leftGain))
        return Error::LeftGainNonFinite;
    if (!std::isfinite(rightGain))
        return Error::RightGainNonFinite;

    const auto candidateLeft = leftInput * leftGain;
    if (!std::isfinite(candidateLeft))
        return Error::LeftProductNonFinite;
    const auto candidateRight = rightInput * rightGain;
    if (!std::isfinite(candidateRight))
        return Error::RightProductNonFinite;

    leftOutput = candidateLeft;
    rightOutput = candidateRight;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> leftInput,
    std::span<const double> rightInput,
    std::optional<std::span<const double>> connectedLeftGain,
    double staticLeftGain,
    std::optional<std::span<const double>> connectedRightGain,
    double staticRightGain,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput,
    bool clipToFullScale = false) noexcept
{
    if (leftInput.size() != rightInput.size()
        || leftInput.size() != leftCandidates.size()
        || leftInput.size() != rightCandidates.size()
        || leftInput.size() != leftOutput.size()
        || leftInput.size() != rightOutput.size()
        || (connectedLeftGain.has_value()
            && leftInput.size() != connectedLeftGain->size())
        || (connectedRightGain.has_value()
            && leftInput.size() != connectedRightGain->size()))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < leftInput.size(); ++frame)
    {
        const auto leftGain = connectedLeftGain.has_value()
            ? (*connectedLeftGain)[frame]
            : staticLeftGain;
        const auto rightGain = connectedRightGain.has_value()
            ? (*connectedRightGain)[frame]
            : staticRightGain;
        const auto error = advance(
            leftInput[frame],
            rightInput[frame],
            leftGain,
            rightGain,
            leftCandidates[frame],
            rightCandidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < leftInput.size(); ++frame)
    {
        // A Gain clips at full scale, as fixed-point signals do.
        leftOutput[frame] = clipToFullScale ? std::clamp(leftCandidates[frame], -1.0, 1.0) : leftCandidates[frame];
        rightOutput[frame] = clipToFullScale ? std::clamp(rightCandidates[frame], -1.0, 1.0) : rightCandidates[frame];
    }
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input,
    std::optional<std::span<const double>> connectedLeftGain,
    double staticLeftGain,
    std::optional<std::span<const double>> connectedRightGain,
    double staticRightGain,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput,
    bool clipToFullScale = false) noexcept
{
    if (input.size() != leftCandidates.size()
        || input.size() != rightCandidates.size()
        || input.size() != leftOutput.size()
        || input.size() != rightOutput.size()
        || (connectedLeftGain.has_value()
            && input.size() != connectedLeftGain->size())
        || (connectedRightGain.has_value()
            && input.size() != connectedRightGain->size()))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto leftGain = connectedLeftGain.has_value()
            ? (*connectedLeftGain)[frame]
            : staticLeftGain;
        const auto rightGain = connectedRightGain.has_value()
            ? (*connectedRightGain)[frame]
            : staticRightGain;
        const auto error = advance(
            input[frame],
            leftGain,
            rightGain,
            leftCandidates[frame],
            rightCandidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        // A Gain clips at full scale, as fixed-point signals do.
        leftOutput[frame] = clipToFullScale ? std::clamp(leftCandidates[frame], -1.0, 1.0) : leftCandidates[frame];
        rightOutput[frame] = clipToFullScale ? std::clamp(rightCandidates[frame], -1.0, 1.0) : rightCandidates[frame];
    }
    return Error::None;
}
}
