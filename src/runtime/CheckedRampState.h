#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace talas::runtime::checked_ramp
{
enum class Variant
{
    Ramp,
    FullRamp,
    RepeatingRamp,
    RepeatingFullRamp,
};

struct State
{
    std::uint64_t elapsedFrames = 0;
    double activeDurationFrames = 1.0;
    bool active = false;
    bool triggerWasPositive = false;
};

struct Controls
{
    double trigger = 0.0;
    double durationSeconds = 0.0;
    double sampleRate = 0.0;
    Variant variant = Variant::Ramp;
};

enum class Error
{
    None,
    SampleRateNonFiniteOrNonPositive,
    TriggerNonFinite,
    DurationNonFiniteOrNonPositive,
    DurationFramesNonFiniteOrOutOfRange,
    ActiveDurationFramesNonFiniteOrOutOfRange,
    ElapsedFramesNonFinite,
    ProgressNonFinite,
    OutputNonFinite,
    VariantUnsupported,
};

[[nodiscard]] inline Error advance(
    const State& state,
    const Controls& controls,
    State& candidate,
    double& output) noexcept
{
    constexpr auto maxRenderableFrames =
        static_cast<double>(std::numeric_limits<long long>::max());
    if (!std::isfinite(controls.sampleRate)
        || controls.sampleRate <= 0.0)
        return Error::SampleRateNonFiniteOrNonPositive;
    if (!std::isfinite(controls.trigger))
        return Error::TriggerNonFinite;
    if (!std::isfinite(controls.durationSeconds)
        || controls.durationSeconds <= 0.0)
        return Error::DurationNonFiniteOrNonPositive;
    const auto durationFrames =
        controls.durationSeconds * controls.sampleRate;
    if (!std::isfinite(durationFrames)
        || durationFrames <= 0.0
        || durationFrames >= maxRenderableFrames)
    {
        return Error::DurationFramesNonFiniteOrOutOfRange;
    }
    if (!std::isfinite(state.activeDurationFrames)
        || state.activeDurationFrames <= 0.0
        || state.activeDurationFrames >= maxRenderableFrames)
    {
        return Error::ActiveDurationFramesNonFiniteOrOutOfRange;
    }

    auto next = state;
    auto candidateOutput = 1.0;
    if (controls.trigger <= 0.0)
    {
        next.elapsedFrames = 0;
        next.active = false;
        next.triggerWasPositive = false;
    }
    else
    {
        if (!next.triggerWasPositive)
        {
            next.active = true;
            next.elapsedFrames = 0;
            next.activeDurationFrames = durationFrames;
        }
        next.triggerWasPositive = true;

        if (next.active)
        {
            const auto elapsedFrames =
                static_cast<double>(next.elapsedFrames);
            if (!std::isfinite(elapsedFrames))
                return Error::ElapsedFramesNonFinite;
            auto progress = 1.0;
            if (next.elapsedFrames < next.activeDurationFrames)
            {
                const auto rawProgress =
                    elapsedFrames / next.activeDurationFrames;
                if (!std::isfinite(rawProgress))
                    return Error::ProgressNonFinite;
                progress = std::fmin(
                    1.0, std::fmax(0.0, rawProgress));
            }

            const auto full =
                controls.variant == Variant::FullRamp
                || controls.variant == Variant::RepeatingFullRamp;
            const auto repeating =
                controls.variant == Variant::RepeatingRamp
                || controls.variant == Variant::RepeatingFullRamp;
            switch (controls.variant)
            {
            case Variant::Ramp:
            case Variant::FullRamp:
            case Variant::RepeatingRamp:
            case Variant::RepeatingFullRamp:
                break;
            default:
                return Error::VariantUnsupported;
            }

            candidateOutput = full
                ? -1.0 + 2.0 * progress
                : progress;
            if (!std::isfinite(candidateOutput))
                return Error::OutputNonFinite;
            if (next.elapsedFrames >= next.activeDurationFrames)
            {
                if (repeating)
                    next.elapsedFrames = 0;
                else
                    next.active = false;
            }
            else
            {
                ++next.elapsedFrames;
            }
        }
    }

    candidate = next;
    output = candidateOutput;
    return Error::None;
}
}
