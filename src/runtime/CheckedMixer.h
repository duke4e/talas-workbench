#pragma once

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::checked_mixer
{
enum class Error
{
    None,
    SizeMismatch,
    Input1NonFinite,
    Gain1NonFinite,
    Product1NonFinite,
    Input2NonFinite,
    Gain2NonFinite,
    Product2NonFinite,
    ResultNonFinite,
};

[[nodiscard]] inline Error advance(
    double input1,
    double gain1,
    double input2,
    double gain2,
    double& output) noexcept
{
    if (!std::isfinite(input1))
        return Error::Input1NonFinite;
    if (!std::isfinite(gain1))
        return Error::Gain1NonFinite;
    if (!std::isfinite(input2))
        return Error::Input2NonFinite;
    if (!std::isfinite(gain2))
        return Error::Gain2NonFinite;

    const auto product1 = input1 * gain1;
    if (!std::isfinite(product1))
        return Error::Product1NonFinite;
    const auto product2 = input2 * gain2;
    if (!std::isfinite(product2))
        return Error::Product2NonFinite;

    const auto result = product1 + product2;
    if (!std::isfinite(result))
        return Error::ResultNonFinite;

    output = result;
    return Error::None;
}

[[nodiscard]] inline Error advanceBlock(
    std::span<const double> input1,
    std::optional<std::span<const double>> connectedGain1,
    double staticGain1,
    std::span<const double> input2,
    std::optional<std::span<const double>> connectedGain2,
    double staticGain2,
    std::span<double> candidates,
    std::span<double> output) noexcept
{
    if (input1.size() != input2.size()
        || input1.size() != candidates.size()
        || input1.size() != output.size()
        || (connectedGain1.has_value()
            && input1.size() != connectedGain1->size())
        || (connectedGain2.has_value()
            && input1.size() != connectedGain2->size()))
    {
        return Error::SizeMismatch;
    }

    for (std::size_t frame = 0; frame < input1.size(); ++frame)
    {
        const auto gain1 = connectedGain1.has_value()
            ? (*connectedGain1)[frame]
            : staticGain1;
        const auto gain2 = connectedGain2.has_value()
            ? (*connectedGain2)[frame]
            : staticGain2;
        const auto error = advance(
            input1[frame], gain1, input2[frame], gain2, candidates[frame]);
        if (error != Error::None)
            return error;
    }

    for (std::size_t frame = 0; frame < input1.size(); ++frame)
        output[frame] = candidates[frame];
    return Error::None;
}
}
