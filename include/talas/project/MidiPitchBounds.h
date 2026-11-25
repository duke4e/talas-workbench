#pragma once

#include "talas/expr/Expression.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>

namespace talas::project
{
[[nodiscard]] inline bool isMidiPitchBound(
    std::string_view nodeType, std::string_view parameterId) noexcept
{
    return (nodeType == "talas.midiMapper" || nodeType == "talas.midiVoice")
        && (parameterId == "lowPitch" || parameterId == "highPitch");
}

// The MIDI-In parameter contract accepts frequencies as well as note numbers.
// Keep this conversion local to those parameters; generic expression domains
// retain their existing meanings. This is a preparation-time, stateless query.
[[nodiscard]] inline talas::expr::EvaluationResult evaluateMidiPitchBound(
    const talas::expr::Expression& expression,
    const talas::expr::EvaluationContext& context)
{
    auto note = talas::expr::evaluate(expression, context,
        {talas::expr::ValueDomain::NoteNumber});
    if (note.ok()) return note;
    const auto frequency = talas::expr::evaluate(expression, context,
        {talas::expr::ValueDomain::Hertz});
    if (!frequency.ok() || frequency.value.value <= 0.0)
        return note;
    const auto converted = 69.0 + 12.0 * std::log2(frequency.value.value / 440.0);
    if (!std::isfinite(converted)) return note;
    return {{converted, talas::expr::ValueDomain::NoteNumber}, {}};
}

// Validate the original interval before rounding. This preserves the existing
// finite 0...127 and ordered-bound contract, including rejection of inverted
// intervals whose rounded endpoints would happen to coincide.
[[nodiscard]] inline std::optional<std::array<std::uint8_t, 2>>
roundedMidiPitchBounds(double low, double high) noexcept
{
    if (!std::isfinite(low) || !std::isfinite(high)
        || low < 0.0 || high > 127.0 || low > high)
        return std::nullopt;
    return std::array{static_cast<std::uint8_t>(std::round(low)),
        static_cast<std::uint8_t>(std::round(high))};
}
}
