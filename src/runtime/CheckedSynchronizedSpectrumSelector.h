#pragma once

#include "talas/dsp/SpectralFrame.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace talas::runtime::checked_synchronized_spectrum_selector
{
// A serialized frame cannot be selected until its final component arrives.
// The output changes on that completion sample, independently of callback size.
[[nodiscard]] inline bool advanceBlock(
    std::span<const talas::dsp::PartialSpectrumFrame> frames,
    talas::dsp::SpectralFrameTimestamp blockStart,
    std::size_t frameLength,
    std::span<const double> connectedTrack,
    std::size_t staticTrack,
    bool amplitudeEnabled,
    bool frequencyEnabled,
    std::span<talas::dsp::PartialSpectrumPoint> retained,
    std::size_t& retainedCount,
    talas::dsp::SpectralFrameTimestamp& nextSample,
    bool& hasNextSample,
    std::span<double> left,
    std::span<double> right) noexcept
{
    if (frameLength == 0 || retained.size() != frameLength
        || retainedCount > frameLength || left.size() != right.size()
        || (!connectedTrack.empty() && connectedTrack.size() != left.size())
        || staticTrack == 0
        || (connectedTrack.empty() && staticTrack > frameLength)
        || left.size() > std::numeric_limits<std::uint64_t>::max()
            - blockStart.absoluteFrame)
    {
        return false;
    }

    const auto blockEnd = blockStart.absoluteFrame + left.size();
    const auto startingCount = hasNextSample && blockStart != nextSample
        ? std::size_t{0} : retainedCount;
    std::uint64_t previousCompletion = 0;
    bool hasPreviousCompletion = false;
    for (const auto& frame : frames)
    {
        if (frame.formatVersion != talas::dsp::partialSpectrumFormatVersion
            || frame.partials.size() != frameLength
            || frame.sourceTimestamp.transportEpoch != blockStart.transportEpoch
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
        if (completion < blockStart.absoluteFrame || completion >= blockEnd
            || (hasPreviousCompletion && completion <= previousCompletion))
        {
            return false;
        }
        previousCompletion = completion;
        hasPreviousCompletion = true;
        for (std::size_t index = 0; index < frameLength; ++index)
        {
            const auto& partial = frame.partials[index];
            if (!partial.hasTrackId || partial.trackId != index + 1
                || !std::isfinite(partial.amplitude)
                || !std::isfinite(partial.frequencyHz)
                || partial.frequencyHz < 0.0
                || !std::isfinite(partial.bandwidthHz))
            {
                return false;
            }
        }
    }

    std::size_t eventIndex = 0;
    auto availablePartials = startingCount;
    for (std::size_t sample = 0; sample < left.size(); ++sample)
    {
        if (eventIndex < frames.size()
            && frames[eventIndex].sourceTimestamp.absoluteFrame
                + frameLength - 1 == blockStart.absoluteFrame + sample)
        {
            availablePartials = frameLength;
            ++eventIndex;
        }
        if (availablePartials == 0)
            continue;
        const auto value = connectedTrack.empty()
            ? static_cast<double>(staticTrack) : connectedTrack[sample];
        if (!std::isfinite(value) || value < 1.0
            || value > static_cast<double>(availablePartials)
            || value != std::round(value))
        {
            return false;
        }
    }

    eventIndex = 0;
    retainedCount = startingCount;
    for (std::size_t sample = 0; sample < left.size(); ++sample)
    {
        if (eventIndex < frames.size()
            && frames[eventIndex].sourceTimestamp.absoluteFrame
                + frameLength - 1 == blockStart.absoluteFrame + sample)
        {
            const auto partials = frames[eventIndex].partials;
            for (std::size_t index = 0; index < frameLength; ++index)
                retained[index] = partials[index];
            retainedCount = frameLength;
            ++eventIndex;
        }
        if (retainedCount == 0)
        {
            left[sample] = 0.0;
            right[sample] = 0.0;
            continue;
        }
        const auto track = connectedTrack.empty()
            ? staticTrack : static_cast<std::size_t>(connectedTrack[sample]);
        const auto& partial = retained[track - 1];
        if (amplitudeEnabled && frequencyEnabled)
        {
            left[sample] = partial.amplitude;
            right[sample] = partial.frequencyHz;
        }
        else if (amplitudeEnabled)
            left[sample] = right[sample] = partial.amplitude;
        else if (frequencyEnabled)
            left[sample] = right[sample] = partial.frequencyHz;
        else
            left[sample] = right[sample] = 0.0;
    }
    nextSample = blockStart;
    nextSample.absoluteFrame = blockEnd;
    hasNextSample = true;
    return true;
}

// Spectra carry frequency as a fraction of half the sample rate: the
// frequency a track selector outputs, on the right or on both channels, is
// divided by it.
[[nodiscard]] inline bool normalizeFrequencyOutputs(
    std::span<double> left,
    std::span<double> right,
    bool amp,
    bool frequency,
    double sampleRate) noexcept
{
    if (!frequency)
        return true;
    const auto nyquist = sampleRate * 0.5;
    if (!std::isfinite(nyquist) || nyquist <= 0.0 || left.size() != right.size())
        return false;
    for (std::size_t frame = 0; frame < right.size(); ++frame)
    {
        const auto rightValue = right[frame] / nyquist;
        const auto leftValue = amp ? left[frame] : left[frame] / nyquist;
        if (!std::isfinite(rightValue) || !std::isfinite(leftValue))
            return false;
        right[frame] = rightValue;
        left[frame] = leftValue;
    }
    return true;
}
}
