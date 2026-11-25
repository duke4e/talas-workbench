#pragma once

#include "CheckedAbsoluteValue.h"
#include "CheckedArcTan.h"
#include "talas/core/PortType.h"

#include <functional>
#include <cmath>
#include <optional>
#include <span>
#include <string_view>

namespace talas::runtime::prepared_unary_control
{
enum class Kind { AbsoluteValue, ArcTan, LinearToLog, LogToLinear };
enum class Error { None, SizeMismatch, OverlappingBuffers, InputNonFinite, InputOutOfDomain, ResultNonFinite };

struct Contract
{
    std::string_view typeId;
    std::string_view displayName;
    std::string_view inputPort = "value";
    std::string_view outputPort = "value";
    talas::core::PortType portType = talas::core::PortType::Control;
};

[[nodiscard]] constexpr std::optional<Contract> contractFor(Kind kind) noexcept
{
    if (kind == Kind::AbsoluteValue) return Contract{"talas.absoluteValue", "Absolute Value"};
    if (kind == Kind::ArcTan) return Contract{"talas.arcTan", "ArcTan"};
    if (kind == Kind::LinearToLog) return Contract{"talas.linearToLog", "Linear To Log"};
    if (kind == Kind::LogToLinear) return Contract{"talas.logToLinear", "Log To Linear"};
    return std::nullopt;
}

// Processing configuration only: callers must retain compiler-owned descriptor,
// graph and buffer authority. This program never grants execution admission.
class Program
{
public:
    [[nodiscard]] static constexpr std::optional<Program> prepare(Kind kind, std::size_t maximumFrames) noexcept
    {
        if (!contractFor(kind)) return std::nullopt;
        return Program{kind, maximumFrames};
    }

    [[nodiscard]] constexpr Contract contract() const noexcept
    {
        return *contractFor(kind_);
    }

    // No mutable processing history: reset and checkpoint have no family-owned
    // work. A failed block leaves this configuration available for exact retry.
    [[nodiscard]] Error process(std::span<const double> input, std::span<double> output) const noexcept
    {
        if (input.size() != output.size() || input.size() > maximumFrames_) return Error::SizeMismatch;
        if (input.empty()) return Error::None;
        // Exact in-place processing is valid. A partial overlap could overwrite
        // an input between validation and commit, so reject it before publishing.
        const auto less = std::less<const double*>{};
        if (input.data() != output.data()
            && less(input.data(), output.data() + output.size())
            && less(output.data(), input.data() + input.size())) return Error::OverlappingBuffers;
        // Reuse the established validate-complete-block, then commit arithmetic
        // order; there is no scratch allocation or mutable state to undo here.
        if (kind_ == Kind::AbsoluteValue)
        {
            switch (checked_absolute_value::advanceBlock(input, output))
            {
                case checked_absolute_value::Error::None: return Error::None;
                case checked_absolute_value::Error::SizeMismatch: return Error::SizeMismatch;
                case checked_absolute_value::Error::InputNonFinite: return Error::InputNonFinite;
                case checked_absolute_value::Error::MagnitudeNonFinite: return Error::ResultNonFinite;
            }
        }
        else if (kind_ == Kind::ArcTan)
        {
            switch (checked_arc_tan::advanceBlock(input, output))
            {
                case checked_arc_tan::Error::None: return Error::None;
                case checked_arc_tan::Error::SizeMismatch: return Error::SizeMismatch;
                case checked_arc_tan::Error::InputNonFinite: return Error::InputNonFinite;
                case checked_arc_tan::Error::AngleNonFinite: return Error::ResultNonFinite;
            }
        }
        else
        {
            for (const auto value : input)
            {
                if (!std::isfinite(value)) return Error::InputNonFinite;
                if (kind_ == Kind::LinearToLog && value <= 0.0)
                    return Error::InputOutOfDomain;
                const auto transformed = kind_ == Kind::LinearToLog
                    ? std::log(value) : std::exp(value);
                if (!std::isfinite(transformed))
                    return Error::ResultNonFinite;
            }
            for (std::size_t frame = 0; frame < input.size(); ++frame)
                output[frame] = kind_ == Kind::LinearToLog
                    ? std::log(input[frame]) : std::exp(input[frame]);
            return Error::None;
        }
        return Error::ResultNonFinite;
    }

private:
    constexpr Program(Kind kind, std::size_t maximumFrames) noexcept : kind_(kind), maximumFrames_(maximumFrames) {}
    Kind kind_;
    std::size_t maximumFrames_;
};
}
