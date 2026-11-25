#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_channeller
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    LeftSelectorNonFinite,
    RightSelectorNonFinite,
};

// A checked channel of Input is output on both
// channels, both checked pass Input unchanged, and neither outputs nothing.
[[nodiscard]] inline Error advance(
    double leftInput,
    double rightInput,
    std::optional<double> leftSelector,
    bool leftFallback,
    std::optional<double> rightSelector,
    bool rightFallback,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(leftInput) || !std::isfinite(rightInput))
        return Error::InputNonFinite;
    if (leftSelector.has_value() && !std::isfinite(*leftSelector))
        return Error::LeftSelectorNonFinite;
    if (rightSelector.has_value() && !std::isfinite(*rightSelector))
        return Error::RightSelectorNonFinite;

    const auto leftEnabled = leftSelector.has_value()
        ? *leftSelector > 0.0
        : leftFallback;
    const auto rightEnabled = rightSelector.has_value()
        ? *rightSelector > 0.0
        : rightFallback;
    leftOutput = leftEnabled ? leftInput : rightEnabled ? rightInput : 0.0;
    rightOutput = rightEnabled ? rightInput : leftEnabled ? leftInput : 0.0;
    return Error::None;
}

// A mono Input has the same left and right channel.
[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input,
    std::span<const double> rightInput,
    std::optional<std::span<const double>> leftSelector,
    bool leftFallback,
    std::optional<std::span<const double>> rightSelector,
    bool rightFallback,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    if ((leftSelector.has_value()
            && leftSelector->size() != input.size())
        || (rightSelector.has_value()
            && rightSelector->size() != input.size())
        || leftCandidates.size() != input.size()
        || rightCandidates.size() != input.size()
        || leftOutput.size() != input.size()
        || rightOutput.size() != input.size()
        || rightInput.size() != input.size())
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto error = advance(
            input[frame],
            rightInput[frame],
            leftSelector.has_value()
                ? std::optional<double>{(*leftSelector)[frame]}
                : std::nullopt,
            leftFallback,
            rightSelector.has_value()
                ? std::optional<double>{(*rightSelector)[frame]}
                : std::nullopt,
            rightFallback,
            leftCandidates[frame],
            rightCandidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        leftOutput[frame] = leftCandidates[frame];
        rightOutput[frame] = rightCandidates[frame];
    }
    return Error::None;
}
}
