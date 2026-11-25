#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace talas::runtime::detail::saturator
{
[[nodiscard]] inline bool validThreshold(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] inline bool validTransitionWidth(double value) noexcept
{
    return std::isfinite(value) && value > 0.0 && value <= 1.0;
}

[[nodiscard]] inline double curve(
    double input, double threshold, double transitionWidth) noexcept
{
    const auto position = std::clamp(
        (input - threshold + transitionWidth)
            / (2.0 * transitionWidth),
        0.0, 1.0);
    return position * position * (3.0 - 2.0 * position);
}

[[nodiscard]] inline bool advanceBlock(
    const std::array<std::optional<std::span<const double>>, 4>& inputs,
    std::optional<std::span<const double>> thresholdInput,
    double fallbackThreshold,
    double transitionWidth,
    bool repressor,
    const std::array<std::span<double>, 4>& outputs) noexcept
{
    if (!inputs[0].has_value()
        || (thresholdInput.has_value()
            && thresholdInput->size() != outputs[0].size())
        || !validTransitionWidth(transitionWidth)
        || (!thresholdInput.has_value()
            && !validThreshold(fallbackThreshold)))
        return false;
    for (std::size_t lane = 0; lane < inputs.size(); ++lane)
    {
        if (outputs[lane].size() != outputs[0].size()
            || (inputs[lane].has_value()
                && inputs[lane]->size() != outputs[0].size()))
            return false;
    }

    for (std::size_t frame = 0; frame < outputs[0].size(); ++frame)
    {
        const auto threshold = thresholdInput.has_value()
            ? (*thresholdInput)[frame] : fallbackThreshold;
        if (!validThreshold(threshold))
            return false;
        for (const auto& input : inputs)
        {
            if (input.has_value() && !std::isfinite((*input)[frame]))
                return false;
        }
    }

    for (std::size_t frame = 0; frame < outputs[0].size(); ++frame)
    {
        const auto threshold = thresholdInput.has_value()
            ? (*thresholdInput)[frame] : fallbackThreshold;
        for (std::size_t lane = 0; lane < inputs.size(); ++lane)
        {
            if (!inputs[lane].has_value())
            {
                outputs[lane][frame] = 0.0;
                continue;
            }
            const auto shaped = curve(
                (*inputs[lane])[frame], threshold, transitionWidth);
            outputs[lane][frame] = repressor ? 1.0 - shaped : shaped;
        }
    }
    return true;
}
}
