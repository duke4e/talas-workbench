#pragma once

#include "talas/runtime/Runtime.h"

#include <cmath>
#include <limits>
#include <optional>
#include <ranges>
#include <string_view>

namespace talas::runtime::detail
{
[[nodiscard]] inline const ParameterValue* findNativeStftParameter(
    const CompileStep& step,
    std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find_if(
        step.parameters,
        [&](const ParameterValue& parameter) {
            return parameter.id == parameterId;
        });
    return found == step.parameters.end() ? nullptr : &*found;
}

[[nodiscard]] inline std::optional<talas::dsp::NativeStftConfiguration>
nativeStftConfiguration(const CompileStep& step) noexcept
{
    const auto* frameSize = findNativeStftParameter(step, "frameSize");
    const auto* hopSize = findNativeStftParameter(step, "hopSize");
    const auto* window = findNativeStftParameter(step, "window");
    if (frameSize == nullptr || hopSize == nullptr || window == nullptr
        || frameSize->choice.has_value() || frameSize->boolean.has_value()
        || frameSize->resource.has_value() || frameSize->text.has_value()
        || hopSize->choice.has_value() || hopSize->boolean.has_value()
        || hopSize->resource.has_value() || hopSize->text.has_value()
        || !window->choice.has_value()
        || !std::isfinite(frameSize->value)
        || !std::isfinite(hopSize->value)
        || frameSize->value != std::round(frameSize->value)
        || hopSize->value != std::round(hopSize->value)
        || frameSize->value < 0.0 || hopSize->value < 0.0
        || static_cast<long double>(frameSize->value)
            >= static_cast<long double>(
                std::numeric_limits<std::size_t>::max())
        || static_cast<long double>(hopSize->value)
            >= static_cast<long double>(
                std::numeric_limits<std::size_t>::max()))
    {
        return std::nullopt;
    }

    talas::dsp::NativeStftConfiguration result;
    result.frameSize = static_cast<std::size_t>(frameSize->value);
    result.hopSize = static_cast<std::size_t>(hopSize->value);
    if (*window->choice == "rectangular")
        result.window = talas::dsp::SpectralWindowKind::Rectangular;
    else if (*window->choice == "hann")
        result.window = talas::dsp::SpectralWindowKind::Hann;
    else
        return std::nullopt;
    return result;
}
}
