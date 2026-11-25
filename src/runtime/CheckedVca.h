#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_vca
{
enum class Error
{
    None,
    SizeMismatch,
    InputNonFinite,
    GainNonFinite,
    ResultNonFinite,
};

[[nodiscard]] inline Error advance(
    double input,
    double gain,
    double& output) noexcept
{
    if (!std::isfinite(input))
        return Error::InputNonFinite;
    if (!std::isfinite(gain))
        return Error::GainNonFinite;

    const auto result = input * gain;
    if (!std::isfinite(result))
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

// A VCA's Inputs multiply together before its gain: multiplies `product`
// by one more Input, in connection order.
[[nodiscard]] inline Error multiplyInput(
    std::span<const double> input,
    std::span<double> product) noexcept
{
    if (input.size() != product.size())
        return Error::SizeMismatch;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        if (!std::isfinite(input[frame]))
            return Error::InputNonFinite;
        const auto result = product[frame] * input[frame];
        if (!std::isfinite(result))
            return Error::ResultNonFinite;
        product[frame] = result;
    }
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input,
    std::optional<std::span<const double>> connectedGain,
    double staticGain,
    std::span<double> candidates,
    std::span<double> output) noexcept
{
    if (input.size() != candidates.size()
        || input.size() != output.size()
        || (connectedGain.has_value()
            && input.size() != connectedGain->size()))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto gain = connectedGain.has_value()
            ? (*connectedGain)[frame]
            : staticGain;
        const auto error = advance(input[frame], gain, candidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < input.size(); ++frame)
        output[frame] = candidates[frame];
    return Error::None;
}
}
