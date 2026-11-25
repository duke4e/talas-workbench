#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace talas::runtime::checked_noise
{
enum class Mode
{
    White,
    Pink,
    // Format 92: largest changes once a period, the spectrum peaking at
    // Frequency.
    HotPink,
    // Projects before format 92: random steps at Frequency, smoothed.
    HotPinkSmoothedSteps,
    // Small changes about a value,
    // a large jump to a new area once a period of Frequency, then small
    // changes about the new value.
    HotPinkOccasionalJumps,
};

struct State
{
    bool initialized = false;
    std::uint64_t randomState = 0;
    double currentRaw = 0.0;
    double hotTarget = 0.0;
    double hotPhase = 0.0;
    double pink0 = 0.0;
    double pink1 = 0.0;
    double pink2 = 0.0;
    double pink3 = 0.0;
    double pink4 = 0.0;
    double pink5 = 0.0;
    double pink6 = 0.0;
    double hotJitter = 0.0;

    bool operator==(const State&) const = default;
};

struct Config
{
    Mode mode = Mode::White;
    double initialState = 0.0;
    double sampleRate = 0.0;
};

struct Controls
{
    double frequency = 0.0;
    double centerValue = 0.0;
    double scale = 0.0;
};

enum class Error
{
    None,
    ConfigInvalid,
    StateInvalid,
    ControlsInvalid,
    PinkStateNonFinite,
    PinkSumNonFinite,
    PhaseIncrementNonFinite,
    PhaseNonFinite,
    PhaseFloorNonFinite,
    SmoothingExponentNonFinite,
    SmoothingNonFinite,
    HotTargetDeltaNonFinite,
    HotSmoothingProductNonFinite,
    HotCorrectionNonFinite,
    RawNonFinite,
    ScaledOutputNonFinite,
    OutputNonFinite,
};

[[nodiscard]] inline bool validMode(Mode mode) noexcept
{
    switch (mode)
    {
    case Mode::White:
    case Mode::Pink:
    case Mode::HotPink:
    case Mode::HotPinkSmoothedSteps:
    case Mode::HotPinkOccasionalJumps:
        return true;
    }
    return false;
}

[[nodiscard]] inline bool validState(const State& state) noexcept
{
    return std::isfinite(state.currentRaw)
        && state.currentRaw >= -1.0 && state.currentRaw <= 1.0
        && std::isfinite(state.hotTarget)
        && state.hotTarget >= -1.0 && state.hotTarget <= 1.0
        && std::isfinite(state.hotPhase)
        && state.hotPhase >= 0.0 && state.hotPhase < 1.0
        && std::isfinite(state.pink0)
        && std::isfinite(state.pink1)
        && std::isfinite(state.pink2)
        && std::isfinite(state.pink3)
        && std::isfinite(state.pink4)
        && std::isfinite(state.pink5)
        && std::isfinite(state.pink6)
        && std::isfinite(state.hotJitter);
}

[[nodiscard]] inline std::uint64_t seed(double initialState) noexcept
{
    const auto normalized =
        (std::clamp(initialState, -1.0, 1.0) + 1.0) * 0.5;
    const auto bucket = static_cast<std::uint64_t>(
        normalized * 9007199254740991.0);
    return bucket ^ 0x8a5cd789635d2dffULL;
}

[[nodiscard]] inline std::uint64_t nextWord(
    std::uint64_t& state) noexcept
{
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] inline double nextUniform(
    std::uint64_t& state) noexcept
{
    constexpr double unitScale = 1.0 / 9007199254740992.0;
    const auto value = nextWord(state) >> 11U;
    return static_cast<double>(value) * unitScale * 2.0 - 1.0;
}

[[nodiscard]] inline Error nextPink(
    State& candidate,
    double& raw) noexcept
{
    const auto white = nextUniform(candidate.randomState);
    const auto pink0 =
        0.99886 * candidate.pink0 + white * 0.0555179;
    const auto pink1 =
        0.99332 * candidate.pink1 + white * 0.0750759;
    const auto pink2 =
        0.96900 * candidate.pink2 + white * 0.1538520;
    const auto pink3 =
        0.86650 * candidate.pink3 + white * 0.3104856;
    const auto pink4 =
        0.55000 * candidate.pink4 + white * 0.5329522;
    const auto pink5 =
        -0.7616 * candidate.pink5 - white * 0.0168980;
    if (!std::isfinite(pink0) || !std::isfinite(pink1)
        || !std::isfinite(pink2) || !std::isfinite(pink3)
        || !std::isfinite(pink4) || !std::isfinite(pink5))
    {
        return Error::PinkStateNonFinite;
    }

    const auto pink = pink0 + pink1 + pink2 + pink3
        + pink4 + pink5 + candidate.pink6 + white * 0.5362;
    if (!std::isfinite(pink))
        return Error::PinkSumNonFinite;
    const auto nextPink6 = white * 0.115926;
    const auto unclampedRaw = pink * 0.11;
    if (!std::isfinite(nextPink6)
        || !std::isfinite(unclampedRaw))
    {
        return Error::PinkStateNonFinite;
    }

    candidate.pink0 = pink0;
    candidate.pink1 = pink1;
    candidate.pink2 = pink2;
    candidate.pink3 = pink3;
    candidate.pink4 = pink4;
    candidate.pink5 = pink5;
    candidate.pink6 = nextPink6;
    raw = std::clamp(unclampedRaw, -1.0, 1.0);
    return Error::None;
}

[[nodiscard]] inline Error nextHotPink(
    State& candidate,
    double frequency,
    double sampleRate,
    double& raw) noexcept
{
    if (frequency <= 0.0)
    {
        raw = candidate.currentRaw;
        return Error::None;
    }

    const auto phaseIncrement = frequency / sampleRate;
    if (!std::isfinite(phaseIncrement))
        return Error::PhaseIncrementNonFinite;
    auto phase = candidate.hotPhase + phaseIncrement;
    if (!std::isfinite(phase))
        return Error::PhaseNonFinite;
    if (phase >= 1.0)
    {
        const auto completedCycles = std::floor(phase);
        if (!std::isfinite(completedCycles))
            return Error::PhaseFloorNonFinite;
        phase -= completedCycles;
        if (!std::isfinite(phase) || phase < 0.0 || phase >= 1.0)
            return Error::PhaseNonFinite;
        candidate.hotTarget = nextUniform(candidate.randomState);
    }

    constexpr double twoPi =
        6.283185307179586476925286766559;
    const auto frequencyProduct = -twoPi * frequency;
    if (!std::isfinite(frequencyProduct))
        return Error::SmoothingExponentNonFinite;
    const auto exponent = frequencyProduct / sampleRate;
    if (!std::isfinite(exponent))
        return Error::SmoothingExponentNonFinite;
    const auto smoothing = std::clamp(
        1.0 - std::exp(exponent), 0.0, 1.0);
    if (!std::isfinite(smoothing))
        return Error::SmoothingNonFinite;

    const auto targetDelta = candidate.hotTarget - candidate.currentRaw;
    if (!std::isfinite(targetDelta))
        return Error::HotTargetDeltaNonFinite;
    const auto smoothedDelta = targetDelta * smoothing;
    if (!std::isfinite(smoothedDelta))
        return Error::HotSmoothingProductNonFinite;
    const auto base = candidate.currentRaw + smoothedDelta;
    if (!std::isfinite(base))
        return Error::HotSmoothingProductNonFinite;
    const auto correctionWhite = nextUniform(candidate.randomState);
    const auto correctionScaled = correctionWhite * smoothing;
    if (!std::isfinite(correctionScaled))
        return Error::HotCorrectionNonFinite;
    const auto correction = correctionScaled * 0.2;
    if (!std::isfinite(correction))
        return Error::HotCorrectionNonFinite;
    const auto unclampedRaw = base + correction;
    if (!std::isfinite(unclampedRaw))
        return Error::RawNonFinite;

    candidate.hotPhase = phase;
    candidate.currentRaw = std::clamp(unclampedRaw, -1.0, 1.0);
    raw = candidate.currentRaw;
    return Error::None;
}

// Format 92 HotPink: a quasi-periodic stream
// whose largest changes come once a period of Frequency. Each period jumps
// to a new height, drawn between half and all of full scale, and glides back
// through zero to its negative, with jitter low-passed at Frequency on top,
// so most changes are small, the spectrum's largest amplitude is at
// Frequency and it falls away above it.
[[nodiscard]] inline Error nextHotPinkPeaked(
    State& candidate,
    double frequency,
    double sampleRate,
    double& raw) noexcept
{
    if (frequency <= 0.0)
    {
        raw = candidate.currentRaw;
        return Error::None;
    }
    const auto phaseIncrement = frequency / sampleRate;
    if (!std::isfinite(phaseIncrement))
        return Error::PhaseIncrementNonFinite;
    auto phase = candidate.hotPhase + phaseIncrement;
    if (!std::isfinite(phase))
        return Error::PhaseNonFinite;
    if (phase >= 1.0)
    {
        const auto completedCycles = std::floor(phase);
        if (!std::isfinite(completedCycles))
            return Error::PhaseFloorNonFinite;
        phase -= completedCycles;
        if (!std::isfinite(phase) || phase < 0.0 || phase >= 1.0)
            return Error::PhaseNonFinite;
        candidate.hotTarget = 0.75 + 0.25 * nextUniform(candidate.randomState);
    }
    constexpr double twoPi = 6.283185307179586476925286766559;
    const auto exponent = -twoPi * frequency / sampleRate;
    if (!std::isfinite(exponent))
        return Error::SmoothingExponentNonFinite;
    const auto smoothing = std::clamp(1.0 - std::exp(exponent), 0.0, 1.0);
    if (!std::isfinite(smoothing))
        return Error::SmoothingNonFinite;
    const auto white = nextUniform(candidate.randomState);
    const auto jitter = candidate.hotJitter + smoothing * (white - candidate.hotJitter);
    if (!std::isfinite(jitter))
        return Error::HotCorrectionNonFinite;
    const auto unclampedRaw = candidate.hotTarget * (1.0 - 2.0 * phase) + 0.1 * jitter;
    if (!std::isfinite(unclampedRaw))
        return Error::RawNonFinite;
    candidate.hotPhase = phase;
    candidate.hotJitter = jitter;
    candidate.currentRaw = std::clamp(unclampedRaw, -1.0, 1.0);
    raw = candidate.currentRaw;
    return Error::None;
}

// Lots of small changes with an occasional
// large jump to a new area, followed by more small changes around the new
// value. Once a period of Frequency the level jumps to a new one drawn
// between -0.9 and 0.9 at least 0.3 from the last; between jumps it stays
// there, with jitter low-passed at Frequency and a tenth of full scale on
// top, so each sample moves it a little.
[[nodiscard]] inline Error nextHotPinkJumps(
    State& candidate,
    double frequency,
    double sampleRate,
    double& raw) noexcept
{
    if (frequency <= 0.0)
    {
        raw = candidate.currentRaw;
        return Error::None;
    }
    const auto phaseIncrement = frequency / sampleRate;
    if (!std::isfinite(phaseIncrement))
        return Error::PhaseIncrementNonFinite;
    auto phase = candidate.hotPhase + phaseIncrement;
    if (!std::isfinite(phase))
        return Error::PhaseNonFinite;
    if (phase >= 1.0)
    {
        const auto completedCycles = std::floor(phase);
        if (!std::isfinite(completedCycles))
            return Error::PhaseFloorNonFinite;
        phase -= completedCycles;
        if (!std::isfinite(phase) || phase < 0.0 || phase >= 1.0)
            return Error::PhaseNonFinite;
        auto target = 0.9 * nextUniform(candidate.randomState);
        if (std::abs(target - candidate.hotTarget) < 0.3)
            target = candidate.hotTarget >= 0.0 ? target - 0.9 : target + 0.9;
        candidate.hotTarget = std::clamp(target, -0.9, 0.9);
    }
    constexpr double twoPi = 6.283185307179586476925286766559;
    const auto exponent = -twoPi * frequency / sampleRate;
    if (!std::isfinite(exponent))
        return Error::SmoothingExponentNonFinite;
    const auto smoothing = std::clamp(1.0 - std::exp(exponent), 0.0, 1.0);
    if (!std::isfinite(smoothing))
        return Error::SmoothingNonFinite;
    const auto white = nextUniform(candidate.randomState);
    const auto jitter = candidate.hotJitter + smoothing * (white - candidate.hotJitter);
    if (!std::isfinite(jitter))
        return Error::HotCorrectionNonFinite;
    const auto unclampedRaw = candidate.hotTarget + 0.1 * jitter;
    if (!std::isfinite(unclampedRaw))
        return Error::RawNonFinite;
    candidate.hotPhase = phase;
    candidate.hotJitter = jitter;
    candidate.currentRaw = std::clamp(unclampedRaw, -1.0, 1.0);
    raw = candidate.currentRaw;
    return Error::None;
}

[[nodiscard]] inline Error advanceFrame(
    Config config,
    Controls controls,
    State state,
    State& nextState,
    double& output) noexcept
{
    if (!validMode(config.mode)
        || !std::isfinite(config.initialState)
        || config.initialState < -1.0 || config.initialState > 1.0
        || !std::isfinite(config.sampleRate)
        || config.sampleRate <= 0.0)
    {
        return Error::ConfigInvalid;
    }
    if (!validState(state))
        return Error::StateInvalid;
    if (!std::isfinite(controls.frequency)
        || controls.frequency < 0.0
        || !std::isfinite(controls.centerValue)
        || !std::isfinite(controls.scale)
        || controls.scale < 0.0)
    {
        return Error::ControlsInvalid;
    }

    auto candidate = state;
    auto raw = 0.0;
    if (!candidate.initialized)
    {
        candidate.initialized = true;
        candidate.randomState = seed(config.initialState);
        candidate.currentRaw = config.initialState;
        candidate.hotTarget = config.initialState;
        raw = candidate.currentRaw;
    }
    else if (config.mode == Mode::White)
    {
        raw = nextUniform(candidate.randomState);
        candidate.currentRaw = raw;
    }
    else if (config.mode == Mode::Pink)
    {
        const auto result = nextPink(candidate, raw);
        if (result != Error::None)
            return result;
        candidate.currentRaw = raw;
    }
    else if (config.mode == Mode::HotPink)
    {
        const auto result = nextHotPinkPeaked(
            candidate, controls.frequency, config.sampleRate, raw);
        if (result != Error::None)
            return result;
    }
    else if (config.mode == Mode::HotPinkOccasionalJumps)
    {
        const auto result = nextHotPinkJumps(
            candidate, controls.frequency, config.sampleRate, raw);
        if (result != Error::None)
            return result;
    }
    else
    {
        const auto result = nextHotPink(
            candidate, controls.frequency, config.sampleRate, raw);
        if (result != Error::None)
            return result;
    }
    if (!std::isfinite(raw) || raw < -1.0 || raw > 1.0)
        return Error::RawNonFinite;

    const auto scaled = controls.scale * raw;
    if (!std::isfinite(scaled))
        return Error::ScaledOutputNonFinite;
    const auto candidateOutput = controls.centerValue + scaled;
    if (!std::isfinite(candidateOutput))
        return Error::OutputNonFinite;

    nextState = candidate;
    output = candidateOutput;
    return Error::None;
}
}
