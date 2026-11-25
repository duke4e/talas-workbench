#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_samples_from_disk_single_step
{
struct State
{
    std::size_t playbackPosition = 0;
    bool playbackActive = false;
    bool playbackStarted = false;
    bool gateWasPositive = false;
};

struct Controls
{
    double filePosition = 0.0;
    double trigger = 1.0;
    double gate = 1.0;
    std::span<const double> samples;
};

enum class Error
{
    None,
    FilePositionInvalid,
    FilePositionOutOfRange,
    TriggerNonFinite,
    GateNonFinite,
    PlaybackPositionOutOfRange,
    SampleNonFinite,
};

[[nodiscard]] inline Error advance(
    const State& state,
    const Controls& controls,
    State& candidate,
    double& output) noexcept
{
    if (!std::isfinite(controls.filePosition)
        || controls.filePosition < 0.0
        || std::trunc(controls.filePosition) != controls.filePosition)
    {
        return Error::FilePositionInvalid;
    }
    if (controls.samples.empty()
        || controls.filePosition
            >= static_cast<double>(controls.samples.size()))
    {
        return Error::FilePositionOutOfRange;
    }
    if (!std::isfinite(controls.trigger))
        return Error::TriggerNonFinite;
    if (!std::isfinite(controls.gate))
        return Error::GateNonFinite;
    if (state.playbackPosition > controls.samples.size())
        return Error::PlaybackPositionOutOfRange;

    const auto selectedPosition =
        static_cast<std::size_t>(controls.filePosition);
    auto next = state;
    auto candidateOutput = 0.0;
    const auto gateIsPositive = controls.gate > 0.0;
    const auto gateRisingEdge =
        gateIsPositive && !state.gateWasPositive;
    next.gateWasPositive = gateIsPositive;

    if (gateRisingEdge
        || (!state.playbackStarted && gateIsPositive))
    {
        next.playbackPosition = selectedPosition;
        next.playbackActive = false;
        next.playbackStarted = false;
    }
    if (!next.playbackStarted && !gateIsPositive)
    {
        next.playbackPosition = selectedPosition;
        candidate = next;
        output = candidateOutput;
        return Error::None;
    }

    if (controls.trigger <= 0.0)
    {
        if (!next.playbackStarted)
        {
            next.playbackPosition = selectedPosition;
        }
        else if (next.playbackPosition != 0)
        {
            const auto lastReadPosition =
                next.playbackPosition < controls.samples.size()
                ? next.playbackPosition - 1
                : controls.samples.size() - 1;
            candidateOutput = controls.samples[lastReadPosition];
            if (!std::isfinite(candidateOutput))
                return Error::SampleNonFinite;
        }
        next.playbackActive = false;
        candidate = next;
        output = candidateOutput;
        return Error::None;
    }

    if (!next.playbackStarted)
    {
        next.playbackPosition = selectedPosition;
        next.playbackActive = true;
        next.playbackStarted = true;
    }
    else if (!next.playbackActive)
    {
        next.playbackActive = true;
    }
    if (next.playbackPosition >= controls.samples.size())
    {
        next.playbackActive = false;
        candidate = next;
        output = candidateOutput;
        return Error::None;
    }

    candidateOutput = controls.samples[next.playbackPosition];
    if (!std::isfinite(candidateOutput))
        return Error::SampleNonFinite;
    ++next.playbackPosition;
    if (next.playbackPosition >= controls.samples.size())
        next.playbackActive = false;

    candidate = next;
    output = candidateOutput;
    return Error::None;
}

[[nodiscard]] inline bool reset(
    State& state,
    std::size_t staticFilePosition,
    std::size_t sampleCount) noexcept
{
    if (sampleCount == 0 || staticFilePosition >= sampleCount)
        return false;
    state = {staticFilePosition, false, false, false};
    return true;
}
}
