#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_pointwise_extremum
{
enum class Kind
{
    Minimum,
    Maximum,
};

enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
};

// Inputs are ordered as three stereo pairs: left, right, left, right, left, right.
[[nodiscard]] inline Error advanceBlock(
    Kind kind,
    const std::array<std::span<const double>, 6>& inputs,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    const auto frames = leftOutput.size();
    if (rightOutput.size() != frames || leftCandidates.size() != frames
        || rightCandidates.size() != frames)
    {
        return Error::SizeMismatch;
    }
    for (const auto input : inputs)
    {
        if (input.size() != frames)
            return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        auto left = inputs[0][frame];
        auto right = inputs[1][frame];
        for (std::size_t index = 0; index < inputs.size(); index += 2)
        {
            const auto candidateLeft = inputs[index][frame];
            const auto candidateRight = inputs[index + 1][frame];
            if (!std::isfinite(candidateLeft)
                || !std::isfinite(candidateRight))
            {
                return Error::InputNonFinite;
            }
            if (kind == Kind::Minimum)
            {
                if (candidateLeft < left)
                    left = candidateLeft;
                if (candidateRight < right)
                    right = candidateRight;
            }
            else
            {
                if (candidateLeft > left)
                    left = candidateLeft;
                if (candidateRight > right)
                    right = candidateRight;
            }
        }
        leftCandidates[frame] = left;
        rightCandidates[frame] = right;
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        leftOutput[frame] = leftCandidates[frame];
        rightOutput[frame] = rightCandidates[frame];
    }
    return Error::None;
}
}
