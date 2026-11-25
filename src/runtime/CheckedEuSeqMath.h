#pragma once

#include <cmath>
#include <cstddef>

namespace talas::runtime::checked_eu_seq
{
struct Config
{
    double sampleRate = 0.0;
    std::size_t steps = 0;
    std::size_t pulses = 0;
    std::size_t rotation = 0;
    std::size_t scaleSteps = 0;
    std::size_t stepsPerOctave = 0;
    double baseFrequency = 0.0;
};

struct State
{
    double phase = 0.0;
    std::size_t step = 0;
    std::size_t nextDegree = 0;
    double frequency = 0.0;
    bool initialized = false;
    bool resetWasPositive = false;
};

struct Frame
{
    double gate = 0.0;
    double frequency = 0.0;
    double step = 0.0;
};

[[nodiscard]] inline bool valid(const Config& config) noexcept
{
    return std::isfinite(config.sampleRate) && config.sampleRate > 0.0
        && config.steps >= 1 && config.steps <= 256
        && config.pulses <= config.steps
        && config.rotation < config.steps
        && config.scaleSteps >= 1 && config.scaleSteps <= 96
        && config.stepsPerOctave >= config.scaleSteps
        && config.stepsPerOctave <= 96
        && std::isfinite(config.baseFrequency)
        && config.baseFrequency > 0.0
        && config.baseFrequency * 2.0 < config.sampleRate * 0.5;
}

[[nodiscard]] inline bool hit(
    const Config& config, std::size_t step) noexcept
{
    if (config.pulses == 0)
        return false;
    const auto shifted =
        (step + config.steps - config.rotation) % config.steps;
    return (shifted * config.pulses) % config.steps < config.pulses;
}

[[nodiscard]] inline double degreeFrequency(
    const Config& config, std::size_t degree) noexcept
{
    const auto octaveStep =
        (degree * config.stepsPerOctave) / config.scaleSteps;
    return config.baseFrequency * std::exp2(
        static_cast<double>(octaveStep)
            / static_cast<double>(config.stepsPerOctave));
}

[[nodiscard]] inline bool advance(
    const Config& config,
    double stepRate,
    double reset,
    const State& state,
    State& next,
    Frame& output) noexcept
{
    if (!valid(config) || !std::isfinite(stepRate)
        || stepRate < 0.0 || stepRate > config.sampleRate * 0.5
        || !std::isfinite(reset)
        || !std::isfinite(state.phase)
        || state.phase < 0.0 || state.phase >= 1.0
        || state.step >= config.steps
        || state.nextDegree >= config.scaleSteps
        || (state.initialized
            && (!std::isfinite(state.frequency)
                || state.frequency <= 0.0)))
        return false;

    next = state;
    if (reset > 0.0 && !state.resetWasPositive)
        next = {};
    next.resetWasPositive = reset > 0.0;
    if (!next.initialized)
    {
        next.frequency = config.baseFrequency;
        if (hit(config, 0))
            next.nextDegree = 1 % config.scaleSteps;
        next.initialized = true;
    }
    output = {
        hit(config, next.step) ? 1.0 : 0.0,
        next.frequency,
        static_cast<double>(next.step),
    };

    next.phase += stepRate / config.sampleRate;
    if (next.phase >= 1.0)
    {
        next.phase -= 1.0;
        next.step = (next.step + 1) % config.steps;
        if (hit(config, next.step))
        {
            next.frequency = degreeFrequency(
                config, next.nextDegree);
            next.nextDegree =
                (next.nextDegree + 1) % config.scaleSteps;
        }
    }
    return std::isfinite(next.phase)
        && next.phase >= 0.0 && next.phase < 1.0
        && std::isfinite(next.frequency)
        && next.frequency > 0.0
        && next.frequency < config.sampleRate * 0.5;
}
}
