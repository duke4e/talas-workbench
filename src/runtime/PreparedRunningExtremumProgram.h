#pragma once

#include "CheckedRunningMinState.h"
#include "CheckedRunningMaxState.h"
#include "talas/core/PortType.h"
#include "talas/runtime/detail/RunningExtremumState.h"

#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string_view>

namespace talas::runtime::prepared_running_extremum
{
using State = detail::RunningExtremumState;
enum class Kind { Minimum, Maximum };
enum class Error { None, SizeMismatch, OverlappingBuffers, ValueNonFinite, ResetNonFinite, MaximumAmplitudeNonFinite };

struct Control
{
    std::span<const double> values;
    double fallback = 0.0;
    [[nodiscard]] double at(std::size_t frame) const noexcept { return values.empty() ? fallback : values[frame]; }
};

struct Contract
{
    std::string_view typeId;
    std::string_view displayName;
    bool minimum;
    std::string_view outputPort = "value";
    talas::core::PortType portType = talas::core::PortType::Control;
    inline static constexpr std::array<std::string_view, 3> inputPorts{"value", "reset", "maximumAmplitude"};
    [[nodiscard]] constexpr std::span<const std::string_view> inputs() const noexcept
    {
        return std::span{inputPorts}.first(minimum ? 3 : 2);
    }
};

[[nodiscard]] constexpr std::optional<Contract> contractFor(Kind kind) noexcept
{
    if (kind == Kind::Minimum) return Contract{"talas.runningMin", "Running Min", true};
    if (kind == Kind::Maximum) return Contract{"talas.runningMax", "Running Max", false};
    return std::nullopt;
}

// Immutable processing configuration, not compiler-owned execution authority.
// State is a value owned by the session; all continuation and rollback paths use
// this same record. No borrowed buffers or mutable pointers survive processing.
class Program
{
public:
    [[nodiscard]] static constexpr std::optional<Program> prepare(Kind kind, std::size_t maximumFrames) noexcept
    {
        if (!contractFor(kind)) return std::nullopt;
        return Program{kind, maximumFrames};
    }
    [[nodiscard]] constexpr Contract contract() const noexcept { return *contractFor(kind_); }
    static void reset(State& state) noexcept { state = {}; }
    [[nodiscard]] static State checkpoint(const State& state) noexcept { return state; }
    static void restore(State& state, const State& checkpoint) noexcept { state = checkpoint; }

    [[nodiscard]] Error process(State& state, std::span<const double> input,
        Control reset, Control maximumAmplitude, std::span<double> output) const noexcept
    {
        const bool minimum = kind_ == Kind::Minimum;
        if (input.size() != output.size() || input.size() > maximumFrames_
            || (!reset.values.empty() && reset.values.size() != input.size())
            || (minimum && !maximumAmplitude.values.empty() && maximumAmplitude.values.size() != input.size()))
            return Error::SizeMismatch;
        if (input.empty()) return Error::None;
        const auto partialOverlap = [&](std::span<const double> values) {
            if (values.empty() || values.data() == output.data()) return false;
            const auto less = std::less<const double*>{};
            return less(values.data(), output.data() + output.size())
                && less(output.data(), values.data() + values.size());
        };
        if (partialOverlap(input) || partialOverlap(reset.values) || (minimum && partialOverlap(maximumAmplitude.values)))
            return Error::OverlappingBuffers;

        // Validate against a stack candidate before publishing any output. Replay
        // from the same checkpoint retains the established checked arithmetic and
        // signed-zero tie order. Selected controls are read before in-place writes.
        auto candidate = checkpoint(state);
        double discarded = 0.0;
        for (std::size_t frame = 0; frame < input.size(); ++frame)
        {
            const auto error = advance(candidate, input[frame], reset.at(frame),
                minimum ? maximumAmplitude.at(frame) : 0.0, discarded);
            if (error != Error::None) return error;
        }
        candidate = checkpoint(state);
        for (std::size_t frame = 0; frame < input.size(); ++frame)
            (void)advance(candidate, input[frame], reset.at(frame),
                minimum ? maximumAmplitude.at(frame) : 0.0, output[frame]);
        state = candidate;
        return Error::None;
    }

private:
    [[nodiscard]] Error advance(State& state, double value, double reset, double amplitude, double& output) const noexcept
    {
        if (kind_ == Kind::Minimum)
        {
            auto candidate = checked_running_min::State{state.initialized, state.extremum};
            switch (checked_running_min::advance(candidate, value, reset, amplitude, output))
            {
                case checked_running_min::Error::ValueNonFinite: return Error::ValueNonFinite;
                case checked_running_min::Error::ResetNonFinite: return Error::ResetNonFinite;
                case checked_running_min::Error::MaximumAmplitudeNonFinite: return Error::MaximumAmplitudeNonFinite;
                case checked_running_min::Error::None: break;
            }
            state = {candidate.initialized, candidate.minimum};
        }
        else
        {
            auto candidate = checked_running_max::State{state.initialized, state.extremum};
            switch (checked_running_max::advance(candidate, value, reset, output))
            {
                case checked_running_max::Error::ValueNonFinite: return Error::ValueNonFinite;
                case checked_running_max::Error::ResetNonFinite: return Error::ResetNonFinite;
                case checked_running_max::Error::None: break;
            }
            state = {candidate.initialized, candidate.maximum};
        }
        return Error::None;
    }
    constexpr Program(Kind kind, std::size_t maximumFrames) noexcept : kind_(kind), maximumFrames_(maximumFrames) {}
    Kind kind_;
    std::size_t maximumFrames_;
};
}
