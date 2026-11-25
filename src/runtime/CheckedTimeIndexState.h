#pragma once

#include "talas/core/SampleLoop.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace talas::runtime::checked_time_index
{
enum class Stage
{
    Idle,
    Active,
    Release,
    Complete,
};

struct Markers
{
    double start = 0.0;
    double attackEnd = 0.0;
    double loopStart = 0.0;
    double loopEnd = 1.0;
    double releaseStart = 1.0;
    double end = 1.0;
};

struct Config
{
    double sampleRate = 0.0;
    double durationSeconds = 1.0;
    double rate = 1.0;
    bool looping = false;
    bool reverse = false;
    Markers markers;
};

struct State
{
    double position = 0.0;
    Stage stage = Stage::Idle;
    bool gateWasPositive = false;
};

enum class Error
{
    None,
    InvalidConfig,
    InvalidMarkers,
    InvalidGate,
    InvalidState,
    NonFiniteStep,
    NonFinitePosition,
};

[[nodiscard]] inline Error validate(const Config& config) noexcept
{
    if (!std::isfinite(config.sampleRate)
        || config.sampleRate <= 0.0
        || !std::isfinite(config.durationSeconds)
        || config.durationSeconds <= 0.0
        || !std::isfinite(config.rate)
        || config.rate < 0.0)
    {
        return Error::InvalidConfig;
    }
    const auto& m = config.markers;
    if (!std::isfinite(m.start)
        || !std::isfinite(m.attackEnd)
        || !std::isfinite(m.loopStart)
        || !std::isfinite(m.loopEnd)
        || !std::isfinite(m.releaseStart)
        || !std::isfinite(m.end)
        || m.start < 0.0
        || m.start > m.attackEnd
        || m.attackEnd > m.loopStart
        || m.loopStart > m.loopEnd
        || m.loopEnd > m.releaseStart
        || m.releaseStart > m.end
        || m.end > 1.0
        || m.start >= m.end
        || (config.looping && m.loopStart >= m.loopEnd))
    {
        return Error::InvalidMarkers;
    }
    return Error::None;
}

// The markers in order, each that passed the one before it saturating at it
// and all within 0 to 1, as a live value saturates at its range (owner,
// 2026-10-12): Start, AttackEnd, LoopStart, LoopEnd, ReleaseStart and End
// in turn, End kept a fixed-point step past Start and, looping, LoopEnd past
// LoopStart. Ordered markers are left as they are.
[[nodiscard]] inline Markers ordered(Markers markers, bool looping) noexcept
{
    constexpr double step = 2.0 / 16777216.0;
    const auto within = [](double value, double low, double high) {
        return std::isfinite(value) ? std::clamp(value, low, high) : value;
    };
    markers.start = within(markers.start, 0.0, 1.0 - step);
    markers.attackEnd = within(markers.attackEnd, markers.start, 1.0);
    markers.loopStart = within(markers.loopStart, markers.attackEnd, 1.0);
    markers.loopEnd = within(markers.loopEnd, markers.loopStart, 1.0);
    markers.releaseStart = within(markers.releaseStart, markers.loopEnd, 1.0);
    markers.end = within(markers.end, std::max(markers.releaseStart, markers.start + step), 1.0);
    if (looping && markers.loopEnd <= markers.loopStart)
    {
        markers.loopEnd = std::min(markers.loopStart + step, 1.0);
        markers.releaseStart = std::max(markers.releaseStart, markers.loopEnd);
        markers.end = std::max(markers.end, markers.releaseStart);
        if (markers.loopEnd <= markers.loopStart)
        {
            markers.loopStart = markers.loopEnd - step;
            markers.attackEnd = std::min(markers.attackEnd, markers.loopStart);
            markers.start = std::min(markers.start, markers.attackEnd);
        }
    }
    return markers;
}

[[nodiscard]] inline std::optional<Config> fromSampleLoop(
    double renderSampleRate,
    std::uint32_t sourceSampleRate,
    std::size_t sourceFrames,
    const talas::core::SampleLoop& loop,
    double rate,
    bool looping,
    bool reverse) noexcept
{
    if (sourceSampleRate == 0 || sourceFrames == 0
        || loop.startFrame >= loop.endFrameExclusive
        || loop.endFrameExclusive > sourceFrames)
        return std::nullopt;
    const auto frameCount = static_cast<double>(sourceFrames);
    Config config{
        renderSampleRate,
        frameCount / static_cast<double>(sourceSampleRate),
        rate,
        looping,
        reverse,
        {0.0, 0.0,
         static_cast<double>(loop.startFrame) / frameCount,
         static_cast<double>(loop.endFrameExclusive) / frameCount,
         1.0, 1.0},
    };
    if (validate(config) != Error::None)
        return std::nullopt;
    return config;
}

// The shared TimeIndex state advances in source-frame/N units. The checked
// sample reader addresses frame j at j/(N-1), so convert only the published
// index. This also lets a loop whose exclusive end is N play frame N-1.
[[nodiscard]] inline std::optional<double> sampleLoopOutput(
    double stageOutput,
    std::size_t sourceFrames,
    bool reverse) noexcept
{
    if (!std::isfinite(stageOutput) || sourceFrames == 0)
        return std::nullopt;
    if (sourceFrames == 1)
        return reverse ? 1.0 : -1.0;
    const auto frames = static_cast<double>(sourceFrames);
    const auto normalized = (stageOutput + 1.0) * 0.5;
    const auto sourceFrame = reverse
        ? normalized * frames - 1.0
        : normalized * frames;
    const auto boundedFrame = std::clamp(
        sourceFrame, 0.0, frames - 1.0);
    const auto output = boundedFrame / (frames - 1.0) * 2.0 - 1.0;
    return std::isfinite(output)
        ? std::optional<double>{output}
        : std::nullopt;
}

// A bounded native stage model for TimeIndex preparation. The public sources
// establish the controls and the trigger/release distinction, but leave exact
// sample phase, loop endpoints and Gate-off timing unspecified.
[[nodiscard]] inline Error advance(
    const State& stateIn,
    const Config& configIn,
    double gate,
    State& candidate,
    double& output) noexcept
{
    // Markers that move out of order saturate against each other, and a
    // marker moved past the position takes it along.
    auto config = configIn;
    config.markers = ordered(config.markers, config.looping);
    const auto configError = validate(config);
    if (configError != Error::None)
        return configError;
    if (!std::isfinite(gate))
        return Error::InvalidGate;
    auto state = stateIn;
    if (state.stage != Stage::Idle && std::isfinite(state.position))
        state.position = std::clamp(state.position, config.markers.start, config.markers.end);
    if (!std::isfinite(state.position)
        || (state.stage != Stage::Idle
            && (state.position < config.markers.start
                || state.position > config.markers.end)))
    {
        return Error::InvalidState;
    }
    switch (state.stage)
    {
    case Stage::Idle:
    case Stage::Active:
    case Stage::Release:
    case Stage::Complete:
        break;
    default:
        return Error::InvalidState;
    }
    const auto durationFrames =
        config.durationSeconds * config.sampleRate;
    if (!std::isfinite(durationFrames) || durationFrames <= 0.0)
        return Error::NonFiniteStep;
    const auto delta = config.rate / durationFrames;
    if (!std::isfinite(delta) || delta < 0.0)
        return Error::NonFiniteStep;

    auto next = state;
    const auto gateIsPositive = gate > 0.0;
    if (gateIsPositive && !next.gateWasPositive)
    {
        next.stage = Stage::Active;
        next.position = config.markers.start;
    }
    else if (!gateIsPositive && next.gateWasPositive
             && next.stage == Stage::Active)
    {
        next.stage = Stage::Release;
        next.position = config.markers.releaseStart;
    }
    next.gateWasPositive = gateIsPositive;

    const auto current = next.stage == Stage::Idle
        ? config.markers.start
        : next.stage == Stage::Complete
            ? config.markers.end
            : next.position;
    const auto normalized = config.reverse
        ? 1.0 - current : current;
    const auto frameOutput = normalized * 2.0 - 1.0;
    if (!std::isfinite(frameOutput))
        return Error::NonFinitePosition;

    if (next.stage == Stage::Active || next.stage == Stage::Release)
    {
        const auto advanced = next.position + delta;
        if (!std::isfinite(advanced))
            return Error::NonFinitePosition;
        if (next.stage == Stage::Active && config.looping
            && advanced >= config.markers.loopEnd)
        {
            const auto loopLength =
                config.markers.loopEnd - config.markers.loopStart;
            const auto offset = std::fmod(
                advanced - config.markers.loopStart, loopLength);
            if (!std::isfinite(offset))
                return Error::NonFinitePosition;
            next.position = config.markers.loopStart + offset;
        }
        else if (advanced >= config.markers.end)
        {
            next.position = config.markers.end;
            next.stage = Stage::Complete;
        }
        else
        {
            next.position = advanced;
        }
    }
    candidate = next;
    output = frameOutput;
    return Error::None;
}
}
