#pragma once

#include "CheckedStaticExpressionArray.h"

#include "talas/dsp/LinearVocoderBank.h"
#include "talas/dsp/ScaleVocoderIntervals.h"
#include "talas/dsp/VocoderBandShaping.h"

#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace talas::runtime::detail
{
// The bank tunings the band fields of a Vocoder, TunableVocoder and
// ScaleVocoder give. Both
// renderers build each block's tuning here from the fields' values, so that
// a hot field retunes the bank alike offline and in realtime. Nothing for
// values the bank cannot take.

// A Vocoder's LoCutoff, HiCutoff and Rolloff shape its Input as a weak tone
// control; projects before format 79 tilt it by Rolloff instead.
struct VocoderInputShaping
{
    double loCutoff = 0.0;
    double hiCutoff = 0.0;
    bool legacyTilt = true;
};

// A Vocoder's HighCF and HiCutoff that pass LowCF and LoCutoff saturate
// against them, as a live value saturates at its range (owner, 2026-10-12,
// taken as recommended on 2026-10-13): HighCF stays a 24-bit step of the
// sample rate above LowCF, so the bands still rise, and HiCutoff stops at
// LoCutoff. Values in order, or not finite, are left as they are.
struct VocoderRanges
{
    double lowCF = 0.0;
    double highCF = 0.0;
    double loCutoff = 0.0;
    double hiCutoff = 0.0;
};

[[nodiscard]] inline VocoderRanges saturatedVocoderRanges(
    double sampleRate, VocoderRanges ranges) noexcept
{
    if (std::isfinite(sampleRate) && sampleRate > 0.0 && std::isfinite(ranges.lowCF)
        && std::isfinite(ranges.highCF) && ranges.highCF <= ranges.lowCF)
    {
        ranges.highCF = ranges.lowCF + sampleRate / 16777216.0;
    }
    if (std::isfinite(ranges.loCutoff) && std::isfinite(ranges.hiCutoff)
        && ranges.hiCutoff < ranges.loCutoff)
    {
        ranges.hiCutoff = ranges.loCutoff;
    }
    return ranges;
}

[[nodiscard]] inline std::optional<talas::dsp::VocoderTuning> vocoderTuning(
    double sampleRate, std::size_t bandCount,
    talas::dsp::VocoderBankFrequencySpacing spacing, double attackSeconds,
    double releaseSeconds, double timeConstant, double lowCF, double highCF,
    double inFreq, double sideFreq, double bandwidth, double rolloff,
    VocoderInputShaping inputShaping) noexcept
{
    talas::dsp::VocoderTuning tuning;
    tuning.configuration = {
        sampleRate,
        bandCount,
        lowCF * inFreq,
        highCF * inFreq,
        lowCF * sideFreq,
        highCF * sideFreq,
        bandwidth,
        bandwidth,
        timeConstant,
        spacing,
        attackSeconds,
        releaseSeconds,
    };
    tuning.tone = 0.0;
    tuning.rolloff = rolloff;
    if (!inputShaping.legacyTilt)
    {
        tuning.shaping = talas::dsp::VocoderBandShapingMode::Cutoffs;
        tuning.loCutoff = inputShaping.loCutoff;
        tuning.hiCutoff = inputShaping.hiCutoff;
    }
    return tuning;
}

// TunableVocoder spacing is in hz between linear centers and in nn (half
// steps) between logarithmic ones; projects before format 44 gave
// a multiple of the base frequency, or octaves.
[[nodiscard]] inline std::optional<talas::dsp::VocoderTuning> tunableVocoderTuning(
    double sampleRate, std::size_t bandCount,
    talas::dsp::VocoderBankFrequencySpacing spacingMode, bool legacyRelativeSpacing,
    double attackSeconds, double releaseSeconds, double timeConstant,
    double analysisFreq, double synthesisFreq, double analysisSpacing,
    double synthesisSpacing, double analysisBandwidth, double synthesisBandwidth,
    double tone, double rolloff, bool legacyTilt) noexcept
{
    const auto deriveRange = legacyRelativeSpacing
        ? &talas::dsp::deriveRelativeVocoderBankFrequencyRange
        : &talas::dsp::deriveVocoderBankFrequencyRange;
    const auto analysisRange = deriveRange(
        analysisFreq, analysisSpacing, bandCount, spacingMode);
    const auto synthesisRange = deriveRange(
        synthesisFreq, synthesisSpacing, bandCount, spacingMode);
    if (!analysisRange.has_value() || !synthesisRange.has_value())
        return std::nullopt;
    talas::dsp::VocoderTuning tuning;
    tuning.configuration = {
        sampleRate,
        bandCount,
        synthesisRange->lowFrequency,
        synthesisRange->highFrequency,
        analysisRange->lowFrequency,
        analysisRange->highFrequency,
        synthesisBandwidth,
        analysisBandwidth,
        timeConstant,
        spacingMode,
        attackSeconds,
        releaseSeconds,
    };
    tuning.tone = tone;
    tuning.rolloff = rolloff;
    // Rolloff sets how far Tone tilts the bank; projects before
    // format 79 tilt it by Tone and again by Rolloff.
    if (!legacyTilt)
        tuning.shaping = talas::dsp::VocoderBandShapingMode::ToneWidth;
    return tuning;
}

// A ScaleVocoder's Intervals in half steps: numbers, or
// expressions within braces, as {!SmallInterval1 rounded nn} is; an
// interval given in live follows Event Values, variables or time, so its
// value there stands. Nothing for text that holds anything else.
[[nodiscard]] inline std::optional<std::vector<double>> scaleVocoderIntervals(
    std::string_view text, double sampleRate, double tempoBpm,
    std::span<const std::optional<double>> live = {})
{
    if (text.size() > talas::dsp::maximumScaleVocoderIntervalTextBytes)
        return std::nullopt;
    auto parsed = checked_static_expression_array::parse(
        text, checked_static_expression_array::Domain::NoteNumber,
        sampleRate, tempoBpm, live);
    if (!parsed.ok())
        return std::nullopt;
    return std::move(parsed.values);
}

[[nodiscard]] inline std::optional<talas::dsp::VocoderTuning> scaleVocoderTuning(
    double sampleRate, std::size_t bandCount, std::span<const double> intervals,
    double attackSeconds, double releaseSeconds, double timeConstant,
    double tonic, double inBandwidth, double sideBandwidth, double tone,
    double rolloff, bool legacyTilt) noexcept
{
    const auto layout = talas::dsp::deriveScaleVocoderIntervalLayout(
        intervals, tonic, bandCount, sampleRate);
    if (layout.status != talas::dsp::ScaleVocoderIntervalStatus::Ok
        || layout.count != bandCount || bandCount == 0)
    {
        return std::nullopt;
    }
    talas::dsp::VocoderTuning tuning;
    tuning.configuration = {
        sampleRate,
        bandCount,
        layout.centerFrequencies[0],
        layout.centerFrequencies[bandCount - 1],
        layout.centerFrequencies[0],
        layout.centerFrequencies[bandCount - 1],
        inBandwidth,
        sideBandwidth,
        timeConstant,
        talas::dsp::VocoderBankFrequencySpacing::Linear,
        attackSeconds,
        releaseSeconds,
    };
    tuning.centers = layout.centerFrequencies;
    tuning.explicitCenters = true;
    tuning.tone = tone;
    tuning.rolloff = rolloff;
    // Rolloff sets the width of the Tone control; projects before
    // format 94 tilt the bands by Tone and again by Rolloff.
    if (!legacyTilt)
        tuning.shaping = talas::dsp::VocoderBandShapingMode::ToneWidth;
    return tuning;
}
}
