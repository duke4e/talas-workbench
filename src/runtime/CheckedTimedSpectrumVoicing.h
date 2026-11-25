#pragma once

#include "talas/dsp/SpectralFrame.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace talas::runtime::checked_timed_spectrum_voicing
{
struct State
{
    bool initialized = false;
    bool stableClassification = false;
    std::size_t transitionSamplesRemaining = 0;
    talas::dsp::SpectralFrameTimestamp nextSample;
    bool hasNextSample = false;
};

[[nodiscard]] inline bool classify(
    std::span<const talas::dsp::PartialSpectrumPoint> partials,
    std::size_t frameLength,
    bool& voiced) noexcept
{
    if (partials.size() != frameLength || frameLength == 0)
        return false;
    const auto fundamental = partials.front().frequencyHz;
    bool positiveAmplitude = false;
    bool harmonic = std::isfinite(fundamental) && fundamental > 0.0;
    for (std::size_t index = 0; index < frameLength; ++index)
    {
        const auto& partial = partials[index];
        if (!partial.hasTrackId || partial.trackId != index + 1
            || !std::isfinite(partial.amplitude)
            || !std::isfinite(partial.frequencyHz)
            || partial.frequencyHz < 0.0
            || !std::isfinite(partial.bandwidthHz))
        {
            return false;
        }
        positiveAmplitude = positiveAmplitude
            || partial.amplitude > 0.0;
        const auto expected = fundamental
            * static_cast<double>(index + 1);
        if (!std::isfinite(expected))
            harmonic = false;
        else
        {
            const auto tolerance = std::max(
                1.0e-9, std::abs(expected) * 1.0e-6);
            if (std::abs(partial.frequencyHz - expected)
                > tolerance)
            {
                harmonic = false;
            }
        }
    }
    voiced = harmonic && positiveAmplitude;
    return true;
}

// A changed classification occupies exactly one serialized spectral period.
// The first complete frame establishes the initial 0/1 value directly.
[[nodiscard]] inline bool advanceBlock(
    std::span<const talas::dsp::PartialSpectrumFrame> frames,
    talas::dsp::SpectralFrameTimestamp blockStart,
    std::size_t frameLength,
    State& state,
    std::span<double> left,
    std::span<double> right) noexcept
{
    if (frameLength == 0 || left.size() != right.size()
        || left.size() > std::numeric_limits<std::uint64_t>::max()
            - blockStart.absoluteFrame)
    {
        return false;
    }
    const auto blockEnd = blockStart.absoluteFrame + left.size();
    std::uint64_t previousCompletion = 0;
    bool hasPreviousCompletion = false;
    for (const auto& frame : frames)
    {
        if (frame.formatVersion
                != talas::dsp::partialSpectrumFormatVersion
            || frame.sourceTimestamp.transportEpoch
                != blockStart.transportEpoch
            || frame.sourceTimestamp.discontinuitySequence
                != blockStart.discontinuitySequence
            || frame.sourceTimestamp.absoluteFrame
                > std::numeric_limits<std::uint64_t>::max()
                    - (frameLength - 1))
        {
            return false;
        }
        const auto completion = frame.sourceTimestamp.absoluteFrame
            + frameLength - 1;
        if (completion < blockStart.absoluteFrame
            || completion >= blockEnd
            || (hasPreviousCompletion
                && completion <= previousCompletion))
        {
            return false;
        }
        bool voiced = false;
        if (!classify(frame.partials, frameLength, voiced))
            return false;
        previousCompletion = completion;
        hasPreviousCompletion = true;
    }

    if (state.hasNextSample && state.nextSample != blockStart)
        state = {};
    std::size_t eventIndex = 0;
    for (std::size_t sample = 0; sample < left.size(); ++sample)
    {
        if (eventIndex < frames.size()
            && frames[eventIndex].sourceTimestamp.absoluteFrame
                + frameLength - 1
                == blockStart.absoluteFrame + sample)
        {
            bool voiced = false;
            (void) classify(frames[eventIndex].partials,
                frameLength, voiced);
            if (state.initialized
                && voiced != state.stableClassification)
            {
                state.transitionSamplesRemaining = frameLength;
            }
            state.initialized = true;
            state.stableClassification = voiced;
            ++eventIndex;
        }
        const auto value = state.transitionSamplesRemaining > 0
            ? 0.5
            : state.initialized && state.stableClassification
                ? 1.0 : 0.0;
        left[sample] = value;
        right[sample] = value;
        if (state.transitionSamplesRemaining > 0)
            --state.transitionSamplesRemaining;
    }
    state.nextSample = blockStart;
    state.nextSample.absoluteFrame = blockEnd;
    state.hasNextSample = true;
    return true;
}
}
