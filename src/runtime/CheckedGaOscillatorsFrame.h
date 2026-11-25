#pragma once

#include "talas/dsp/GaTable.h"

#include <cmath>
#include <cstddef>
#include <span>

namespace talas::runtime::checked_ga_oscillators
{
// One frame of a GAOscillators: each partial's amplitude, crossfaded from
// analysis0 toward analysis1 by Morph, at its phase, summed and scaled by
// Envelope; each phase then advances by its harmonic of Frequency. An empty
// analysis1 holds analysis0. Both renderers call this one out-of-line
// definition, so they round each frame alike. Fails when a value is not
// finite, nextPhases then incomplete; it may be phases itself.
[[gnu::noinline]] inline bool renderFrame(
    std::span<const double> analysis0,
    std::span<const double> analysis1,
    double frequency,
    double morph,
    double envelope,
    double sampleRate,
    std::span<const double> phases,
    std::span<double> nextPhases,
    double& output) noexcept
{
    constexpr double twoPi = 6.283185307179586476925286766559;
    if (phases.size() != analysis0.size() || nextPhases.size() != analysis0.size()
        || (!analysis1.empty() && analysis1.size() != analysis0.size()))
    {
        return false;
    }
    double sample = 0.0;
    for (std::size_t partial = 0; partial < analysis0.size(); ++partial)
    {
        const auto from = analysis0[partial];
        const auto to = analysis1.empty() ? from : analysis1[partial];
        const auto amplitude = from + (to - from) * morph;
        sample += amplitude * std::sin(phases[partial]);

        const auto partialFrequency = frequency * static_cast<double>(partial + 1);
        auto next = phases[partial] + twoPi * partialFrequency / sampleRate;
        if (next >= twoPi || next <= -twoPi)
            next = std::fmod(next, twoPi);
        if (!std::isfinite(amplitude) || !std::isfinite(sample) || !std::isfinite(next))
            return false;
        nextPhases[partial] = next;
    }
    output = sample * envelope;
    return std::isfinite(output);
}

// One frame of a GAOscillators on GA analyses (ADR-0027): each
// oscillator's complex waveform, crossfaded from
// analysis0's toward analysis1's by Morph and read at the shared phase
// (linearly between its 4096 samples), times its amplitude envelope at
// TimeIndex, crossfaded alike, summed and scaled by Envelope; the phase, in
// cycles, then advances by Frequency times the frequency deviation at
// TimeIndex, crossfaded alike. Each analysis's envelopes are read at
// TimeIndex over their own length. Both analyses must hold as many
// waveforms (analysis1 may be absent: it holds analysis0).
[[gnu::noinline]] inline bool renderGaFrame(
    const talas::dsp::ga::View& analysis0,
    const talas::dsp::ga::View* analysis1,
    double timeIndex,
    double frequency,
    double morph,
    double envelope,
    double sampleRate,
    double phase,
    double& nextPhase,
    double& output) noexcept
{
    const auto& other = analysis1 == nullptr ? analysis0 : *analysis1;
    if (other.waveforms != analysis0.waveforms || !std::isfinite(phase))
        return false;
    constexpr auto length = talas::dsp::ga::waveformLength;
    const auto position = phase * static_cast<double>(length);
    const auto below = static_cast<std::size_t>(std::floor(position)) % length;
    const auto above = (below + 1) % length;
    const auto fraction = position - std::floor(position);
    double sample = 0.0;
    for (std::size_t index = 0; index < analysis0.waveforms; ++index)
    {
        const auto read = [&](const talas::dsp::ga::View& view) {
            const auto from = view.waveform(index, below);
            return from + (view.waveform(index, above) - from) * fraction;
        };
        const auto level = [&](const talas::dsp::ga::View& view) {
            return talas::dsp::ga::atTimeIndex(view.envelopeLength, timeIndex,
                [&](std::size_t frame) { return view.amplitude(index, frame); });
        };
        const auto from = read(analysis0);
        const auto wave = from + (read(other) - from) * morph;
        const auto start = level(analysis0);
        const auto amplitude = start + (level(other) - start) * morph;
        sample += amplitude * wave;
    }
    const auto deviation = [&](const talas::dsp::ga::View& view) {
        return talas::dsp::ga::atTimeIndex(view.envelopeLength, timeIndex,
            [&](std::size_t frame) { return view.deviation(frame); });
    };
    const auto bend = deviation(analysis0);
    const auto pitch = frequency * (bend + (deviation(other) - bend) * morph);
    auto next = phase + pitch / sampleRate;
    next -= std::floor(next);
    output = sample * envelope;
    if (!std::isfinite(output) || !std::isfinite(next))
        return false;
    nextPhase = next;
    return true;
}
}
