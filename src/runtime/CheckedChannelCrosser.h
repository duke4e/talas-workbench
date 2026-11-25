#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_channel_crosser
{
enum class Error
{
    None,
    SizeMismatch,
    LeftInputNonFinite,
    RightInputNonFinite,
    LeftInLeftNonFinite,
    RightInLeftNonFinite,
    LeftInRightNonFinite,
    RightInRightNonFinite,
    LeftFromLeftNonFinite,
    LeftFromRightNonFinite,
    RightFromLeftNonFinite,
    RightFromRightNonFinite,
    LeftSumNonFinite,
    RightSumNonFinite,
};

[[nodiscard]] inline Error advance(
    double leftInput,
    double rightInput,
    const std::array<double, 4>& coefficients,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(leftInput))
        return Error::LeftInputNonFinite;
    if (!std::isfinite(rightInput))
        return Error::RightInputNonFinite;
    if (!std::isfinite(coefficients[0]))
        return Error::LeftInLeftNonFinite;
    if (!std::isfinite(coefficients[1]))
        return Error::RightInLeftNonFinite;
    if (!std::isfinite(coefficients[2]))
        return Error::LeftInRightNonFinite;
    if (!std::isfinite(coefficients[3]))
        return Error::RightInRightNonFinite;

    const auto leftFromLeft = leftInput * coefficients[0];
    if (!std::isfinite(leftFromLeft))
        return Error::LeftFromLeftNonFinite;
    const auto leftFromRight = rightInput * coefficients[1];
    if (!std::isfinite(leftFromRight))
        return Error::LeftFromRightNonFinite;
    const auto rightFromLeft = leftInput * coefficients[2];
    if (!std::isfinite(rightFromLeft))
        return Error::RightFromLeftNonFinite;
    const auto rightFromRight = rightInput * coefficients[3];
    if (!std::isfinite(rightFromRight))
        return Error::RightFromRightNonFinite;

    const auto candidateLeft = leftFromLeft + leftFromRight;
    if (!std::isfinite(candidateLeft))
        return Error::LeftSumNonFinite;
    const auto candidateRight = rightFromLeft + rightFromRight;
    if (!std::isfinite(candidateRight))
        return Error::RightSumNonFinite;

    leftOutput = candidateLeft;
    rightOutput = candidateRight;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> leftInput,
    std::optional<std::span<const double>> rightInput,
    const std::array<std::optional<std::span<const double>>, 4>&
        connectedCoefficients,
    const std::array<double, 4>& staticCoefficients,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    if ((rightInput.has_value()
            && leftInput.size() != rightInput->size())
        || leftInput.size() != leftCandidates.size()
        || leftInput.size() != rightCandidates.size()
        || leftInput.size() != leftOutput.size()
        || leftInput.size() != rightOutput.size())
    {
        return Error::SizeMismatch;
    }
    for (const auto& connected : connectedCoefficients)
    {
        if (connected.has_value()
            && leftInput.size() != connected->size())
        {
            return Error::SizeMismatch;
        }
    }

    for (std::size_t frame = 0; frame < leftInput.size(); ++frame)
    {
        std::array<double, 4> coefficients{};
        for (std::size_t index = 0; index < coefficients.size(); ++index)
        {
            coefficients[index] = connectedCoefficients[index].has_value()
                ? (*connectedCoefficients[index])[frame]
                : staticCoefficients[index];
        }
        const auto error = advance(
            leftInput[frame],
            rightInput.has_value() ? (*rightInput)[frame] : 0.0,
            coefficients,
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
