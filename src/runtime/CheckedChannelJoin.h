#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_channel_join
{
enum class Error
{
    None,
    SizeMismatch,
    LeftInputNonFinite,
    RightInputNonFinite,
};

[[nodiscard]] inline Error advance(
    double leftInput,
    double rightInput,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(leftInput))
        return Error::LeftInputNonFinite;
    if (!std::isfinite(rightInput))
        return Error::RightInputNonFinite;

    leftOutput = leftInput;
    rightOutput = rightInput;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> leftInput,
    std::span<const double> rightInput,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    if (leftInput.size() != rightInput.size()
        || leftInput.size() != leftCandidates.size()
        || leftInput.size() != rightCandidates.size()
        || leftInput.size() != leftOutput.size()
        || leftInput.size() != rightOutput.size())
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < leftInput.size(); ++frame)
    {
        const auto error = advance(
            leftInput[frame],
            rightInput[frame],
            leftCandidates[frame],
            rightCandidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < leftInput.size(); ++frame)
    {
        leftOutput[frame] = leftCandidates[frame];
        rightOutput[frame] = rightCandidates[frame];
    }
    return Error::None;
}
}
