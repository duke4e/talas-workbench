#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_triggered_table_read
{
struct State
{
    std::size_t readIndex = 0;
    double heldValue = 0.0;
    bool gateWasPositive = false;
};

struct Controls
{
    double trigger = 1.0;
    double gate = 1.0;
    std::span<const double> samples;
    bool ignoreLoops = false;
    // The loop in the table's header as frames [loopStart, loopEnd); a
    // table without one plays once. Projects before format 71 looped the
    // whole table instead.
    std::size_t loopStart = 0;
    std::size_t loopEnd = 0;
    bool legacyWholeTableLoop = false;
};

enum class Error
{
    None,
    TriggerNonFinite,
    GateNonFinite,
    TableEmpty,
    ReadIndexOutOfRange,
    HeldValueNonFinite,
    SampleNonFinite,
};

[[nodiscard]] inline Error advance(
    const State& state,
    const Controls& controls,
    State& candidate,
    double& output) noexcept
{
    if (!std::isfinite(controls.trigger))
        return Error::TriggerNonFinite;
    if (!std::isfinite(controls.gate))
        return Error::GateNonFinite;
    if (controls.samples.empty())
        return Error::TableEmpty;
    if (state.readIndex > controls.samples.size())
        return Error::ReadIndexOutOfRange;
    if (!std::isfinite(state.heldValue))
        return Error::HeldValueNonFinite;

    auto next = state;
    const auto gateIsPositive = controls.gate > 0.0;
    if (gateIsPositive && !state.gateWasPositive)
    {
        next.readIndex = 0;
        next.heldValue = 0.0;
    }
    next.gateWasPositive = gateIsPositive;

    if (controls.trigger > 0.0)
    {
        const auto loopStart = controls.legacyWholeTableLoop
            ? std::size_t{0} : controls.loopStart;
        const auto loopEnd = controls.legacyWholeTableLoop
            ? controls.samples.size() : controls.loopEnd;
        if (!controls.ignoreLoops && loopStart < loopEnd
            && loopEnd <= controls.samples.size()
            && next.readIndex >= loopEnd)
        {
            next.readIndex = loopStart;
        }

        if (next.readIndex < controls.samples.size())
        {
            const auto sample = controls.samples[next.readIndex];
            if (!std::isfinite(sample))
                return Error::SampleNonFinite;
            next.heldValue = sample;
            ++next.readIndex;
        }
        else
        {
            const auto sample = controls.samples.back();
            if (!std::isfinite(sample))
                return Error::SampleNonFinite;
            next.heldValue = sample;
        }
    }

    candidate = next;
    output = next.heldValue;
    return Error::None;
}
}
