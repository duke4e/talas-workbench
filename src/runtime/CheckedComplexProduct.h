#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_complex_product
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    ProductNonFinite,
    OutputNonFinite,
};

[[nodiscard]] inline Error advance(
    double input0Left,
    double input0Right,
    double input1Left,
    double input1Right,
    double& leftOutput,
    double& rightOutput) noexcept
{
    if (!std::isfinite(input0Left) || !std::isfinite(input0Right)
        || !std::isfinite(input1Left) || !std::isfinite(input1Right))
    {
        return Error::InputNonFinite;
    }

    const auto real0 = input0Left * input1Left;
    const auto real1 = input0Right * input1Right;
    const auto imaginary0 = input0Left * input1Right;
    const auto imaginary1 = input0Right * input1Left;
    if (!std::isfinite(real0) || !std::isfinite(real1)
        || !std::isfinite(imaginary0) || !std::isfinite(imaginary1))
    {
        return Error::ProductNonFinite;
    }

    const auto real = real0 - real1;
    const auto imaginary = imaginary0 + imaginary1;
    if (!std::isfinite(real) || !std::isfinite(imaginary))
        return Error::OutputNonFinite;
    leftOutput = real;
    rightOutput = imaginary;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input0Left,
    std::span<const double> input0Right,
    std::span<const double> input1Left,
    std::span<const double> input1Right,
    std::span<double> leftCandidates,
    std::span<double> rightCandidates,
    std::span<double> leftOutput,
    std::span<double> rightOutput) noexcept
{
    const auto frames = input0Left.size();
    if (input0Right.size() != frames || input1Left.size() != frames
        || input1Right.size() != frames || leftCandidates.size() != frames
        || rightCandidates.size() != frames || leftOutput.size() != frames
        || rightOutput.size() != frames)
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        const auto error = advance(
            input0Left[frame], input0Right[frame],
            input1Left[frame], input1Right[frame],
            leftCandidates[frame], rightCandidates[frame]);
        if (error != Error::None)
            return error;
    }
    for (std::size_t frame = 0; frame < frames; ++frame)
    {
        leftOutput[frame] = leftCandidates[frame];
        rightOutput[frame] = rightCandidates[frame];
    }
    return Error::None;
}
}
