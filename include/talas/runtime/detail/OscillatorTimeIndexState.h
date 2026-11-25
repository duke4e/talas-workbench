#pragma once

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::detail::oscillator_time_index
{
struct State
{
    double phase = 0.0;
};

[[nodiscard]] inline bool advanceBlock(
    State& state,
    std::span<const double> frequencyInput,
    double fallbackFrequency,
    double sampleRate,
    std::span<double> output) noexcept
{
    if (!std::isfinite(state.phase) || state.phase < 0.0
        || state.phase >= 1.0 || !std::isfinite(sampleRate)
        || sampleRate <= 0.0
        || (!frequencyInput.empty()
            && frequencyInput.size() != output.size()))
        return false;

    const auto validFrequency = [sampleRate](double frequency) {
        return std::isfinite(frequency) && frequency >= 0.0
            && frequency <= sampleRate * 0.5;
    };
    if (frequencyInput.empty() && !validFrequency(fallbackFrequency))
        return false;
    for (const auto frequency : frequencyInput)
        if (!validFrequency(frequency))
            return false;

    auto phase = state.phase;
    for (std::size_t frame = 0; frame < output.size(); ++frame)
    {
        output[frame] = 2.0 * phase - 1.0;
        const auto frequency = frequencyInput.empty()
            ? fallbackFrequency : frequencyInput[frame];
        phase += frequency / sampleRate;
        if (phase >= 1.0)
            phase -= 1.0;
    }
    state.phase = phase;
    return true;
}
}
