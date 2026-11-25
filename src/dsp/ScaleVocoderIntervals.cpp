#include "talas/dsp/ScaleVocoderIntervals.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <system_error>

namespace talas::dsp
{
namespace
{
[[nodiscard]] bool asciiWhitespace(char character) noexcept
{
    return character == ' ' || character == '\t' || character == '\n'
        || character == '\r' || character == '\f' || character == '\v';
}

// Takes the next of a scale's intervals, as it lies between -128 and 128
// semitones and above the one before.
[[nodiscard]] bool appendInterval(
    ScaleVocoderIntervalLayout& result, double interval) noexcept
{
    if (interval < -128.0 || interval > 128.0)
    {
        result.status = ScaleVocoderIntervalStatus::IntervalOutOfRange;
        return false;
    }
    if (result.count != 0
        && interval <= result.semitoneOffsets[result.count - 1])
    {
        result.status =
            ScaleVocoderIntervalStatus::IntervalsNotIncreasing;
        return false;
    }
    result.semitoneOffsets[result.count++] = interval;
    return true;
}

// Repeats the scale each octave for as many bands as there are
// and places their centers from the Tonic.
[[nodiscard]] ScaleVocoderIntervalLayout completeLayout(
    ScaleVocoderIntervalLayout result,
    double tonicMidiNote,
    std::size_t expectedBandCount,
    double sampleRate) noexcept
{
    if (result.count == 0)
    {
        result.status = ScaleVocoderIntervalStatus::InvalidCount;
        return result;
    }
    // The scale may have any number of steps; the bands take its
    // first ones.
    result.count = std::min(result.count, expectedBandCount);
    const auto patternCount = result.count;
    for (std::size_t index = patternCount; index < expectedBandCount; ++index)
    {
        result.semitoneOffsets[index] =
            result.semitoneOffsets[index % patternCount]
            + 12.0 * static_cast<double>(index / patternCount);
    }
    result.count = expectedBandCount;

    const auto nyquist = sampleRate * 0.5;
    for (std::size_t index = 0; index < result.count; ++index)
    {
        const auto frequency = 440.0 * std::exp2(
            (tonicMidiNote + result.semitoneOffsets[index] - 69.0) / 12.0);
        if (!std::isfinite(frequency) || frequency <= 0.0
            || frequency >= nyquist
            || (index != 0
                && frequency <= result.centerFrequencies[index - 1]))
        {
            result.status =
                ScaleVocoderIntervalStatus::InvalidDerivedFrequency;
            return result;
        }
        result.centerFrequencies[index] = frequency;
    }
    result.status = ScaleVocoderIntervalStatus::Ok;
    return result;
}

// The checks before any interval, in the order the text form makes them.
[[nodiscard]] bool admitsLayout(
    ScaleVocoderIntervalLayout& result,
    double tonicMidiNote,
    std::size_t expectedBandCount,
    double sampleRate) noexcept
{
    if (expectedBandCount == 0
        || expectedBandCount > maximumLinearVocoderBandCount)
    {
        result.status = ScaleVocoderIntervalStatus::InvalidCount;
        return false;
    }
    if (!std::isfinite(tonicMidiNote))
    {
        result.status = ScaleVocoderIntervalStatus::InvalidTonic;
        return false;
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
    {
        result.status = ScaleVocoderIntervalStatus::InvalidDerivedFrequency;
        return false;
    }
    return true;
}
}

ScaleVocoderIntervalLayout deriveScaleVocoderIntervalLayout(
    std::string_view text,
    double tonicMidiNote,
    std::size_t expectedBandCount,
    double sampleRate) noexcept
{
    ScaleVocoderIntervalLayout result;
    if (text.size() > maximumScaleVocoderIntervalTextBytes)
    {
        result.status = ScaleVocoderIntervalStatus::TextTooLong;
        return result;
    }
    if (!admitsLayout(result, tonicMidiNote, expectedBandCount, sampleRate))
        return result;

    std::size_t position = 0;
    while (position < text.size())
    {
        while (position < text.size() && asciiWhitespace(text[position]))
            ++position;
        if (position == text.size())
            break;
        auto first = text.data() + position;
        auto endPosition = position;
        while (endPosition < text.size()
            && !asciiWhitespace(text[endPosition]))
        {
            ++endPosition;
        }
        if (result.count >= maximumLinearVocoderBandCount)
        {
            result.status = ScaleVocoderIntervalStatus::InvalidCount;
            return result;
        }
        if (*first == '+')
        {
            ++first;
            if (first == text.data() + endPosition)
            {
                result.status = ScaleVocoderIntervalStatus::InvalidToken;
                return result;
            }
        }
        double interval = 0.0;
        const auto parsed = std::from_chars(
            first,
            text.data() + endPosition,
            interval,
            std::chars_format::general);
        if (parsed.ec != std::errc{}
            || parsed.ptr != text.data() + endPosition
            || !std::isfinite(interval))
        {
            result.status = ScaleVocoderIntervalStatus::InvalidToken;
            return result;
        }
        if (!appendInterval(result, interval))
            return result;
        position = endPosition;
    }
    return completeLayout(result, tonicMidiNote, expectedBandCount, sampleRate);
}

ScaleVocoderIntervalLayout deriveScaleVocoderIntervalLayout(
    std::span<const double> intervals,
    double tonicMidiNote,
    std::size_t expectedBandCount,
    double sampleRate) noexcept
{
    ScaleVocoderIntervalLayout result;
    if (!admitsLayout(result, tonicMidiNote, expectedBandCount, sampleRate))
        return result;
    for (const auto interval : intervals)
    {
        if (result.count >= maximumLinearVocoderBandCount)
        {
            result.status = ScaleVocoderIntervalStatus::InvalidCount;
            return result;
        }
        if (!std::isfinite(interval))
        {
            result.status = ScaleVocoderIntervalStatus::InvalidToken;
            return result;
        }
        if (!appendInterval(result, interval))
            return result;
    }
    return completeLayout(result, tonicMidiNote, expectedBandCount, sampleRate);
}

std::string_view scaleVocoderIntervalStatusMessage(
    ScaleVocoderIntervalStatus status) noexcept
{
    switch (status)
    {
        case ScaleVocoderIntervalStatus::Ok:
            return "ok";
        case ScaleVocoderIntervalStatus::TextTooLong:
            return "interval text exceeds 2048 bytes";
        case ScaleVocoderIntervalStatus::InvalidCount:
            return "needs 1 to nbrBands intervals and 1 to 64 bands";
        case ScaleVocoderIntervalStatus::InvalidToken:
            return "intervals must be finite whitespace-separated decimal semitone values";
        case ScaleVocoderIntervalStatus::IntervalOutOfRange:
            return "interval values must be between -128 and 128 semitones";
        case ScaleVocoderIntervalStatus::IntervalsNotIncreasing:
            return "interval values must be strictly increasing";
        case ScaleVocoderIntervalStatus::InvalidTonic:
            return "tonic must be finite";
        case ScaleVocoderIntervalStatus::InvalidDerivedFrequency:
            return "derived centers must be finite, positive, increasing, and below Nyquist";
    }
    return "unknown interval error";
}
}
