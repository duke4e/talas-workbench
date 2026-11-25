#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>

namespace talas::runtime::live_field_ranges
{
// A live value outside a field's documented range saturates to the range's
// edge in both renderers, as the documented cases do (a MultiSegment
// BreakPoint sticks at the value of 1; a Probability above 1 is clipped
// to 1). A whole-number field's live
// value is then rounded to the nearest whole number. A value inside the range,
// and a whole one, is left as it is, so only values both renderers used to
// refuse change; a field's static value is still checked when it compiles.
// CDP fields keep CDP8's refusal of a breakpoint value outside its range.
//
// An open edge (a time or frequency that must stay above zero or below
// Nyquist) saturates one step inside it: a sample for a time, a 24-bit step
// of the sample rate for a frequency, a 24-bit step of unity for a ratio,
// the resolution of 24-bit fixed point.
enum class Unit
{
    Scalar,
    Seconds,
    Hertz,
};

struct Range
{
    std::string_view typeId;
    std::string_view parameterId;
    Unit unit = Unit::Scalar;
    double low = -std::numeric_limits<double>::infinity();
    bool lowOpen = false;
    double high = std::numeric_limits<double>::infinity();
    bool highOpen = false;
    // When positive, the high edge is this fraction of the sample rate.
    double highPerSampleRate = 0.0;
    // When positive, the low edge is this fraction of the sample rate.
    double lowPerSampleRate = 0.0;
    // A count or position taking whole numbers: a live value is rounded to
    // the nearest one once it is within the range.
    bool whole = false;
    // A time whose -1 means "inherit the TimeConstant": a live value of
    // exactly -1 inherits, and any other
    // saturates at the range.
    bool inheritsAtMinusOne = false;
    // A rate that plays either way but never stops (a DiskPlayer's
    // RateScale): a live value of exactly zero saturates one step above it.
    bool nonZero = false;
};

namespace detail
{
inline constexpr double unbounded = std::numeric_limits<double>::infinity();

// A time above zero.
constexpr Range time(std::string_view type, std::string_view id) noexcept
{
    return {type, id, Unit::Seconds, 0.0, true, unbounded, false, 0.0};
}

// A frequency above zero, below a fraction of the sample rate when one is
// given.
constexpr Range frequency(std::string_view type, std::string_view id, double belowRate = 0.0) noexcept
{
    return {type, id, Unit::Hertz, 0.0, true, unbounded, belowRate > 0.0, belowRate};
}

// A ratio, rate or width above zero, up to a maximum when one is given.
constexpr Range positive(std::string_view type, std::string_view id, double upTo = unbounded) noexcept
{
    return {type, id, Unit::Scalar, 0.0, true, upTo, false, 0.0};
}

constexpr Range closed(std::string_view type, std::string_view id, double low, double high,
    Unit unit = Unit::Scalar) noexcept
{
    return {type, id, unit, low, false, high, false, 0.0};
}

constexpr Range unit(std::string_view type, std::string_view id) noexcept
{
    return closed(type, id, 0.0, 1.0);
}

constexpr Range bipolar(std::string_view type, std::string_view id) noexcept
{
    return closed(type, id, -1.0, 1.0);
}

// A whole number from low to high.
constexpr Range whole(std::string_view type, std::string_view id, double low, double high) noexcept
{
    return {type, id, Unit::Scalar, low, false, high, false, 0.0, 0.0, true};
}

// A rate of either sign that is never zero.
constexpr Range nonZeroRate(std::string_view type, std::string_view id) noexcept
{
    return {type, id, Unit::Scalar, -unbounded, false, unbounded, false, 0.0, 0.0, false, false, true};
}

// A vocoder's envelope time: -1 inherits, otherwise a time from zero.
constexpr Range inheritingTime(std::string_view type, std::string_view id) noexcept
{
    return {type, id, Unit::Seconds, 0.0, false, unbounded, false, 0.0, 0.0, false, true};
}
}

inline constexpr std::array ranges{
    detail::frequency("talas.analysisFilter", "fundamental"),
    detail::frequency("talas.averagingLowPassFilter", "cutoff"),
    detail::frequency("talas.frequencyTracker", "maxFrequency", 0.5),
    detail::frequency("talas.frequencyTracker", "minFrequency", 0.5),
    // An Interpolate's mixes are limited to 0 to 1, so that 2 * !Morph
    // clips at 1 and 2 * !Morph - 1 at 0.
    detail::unit("talas.interpolate", "amount"),
    detail::unit("talas.interpolate", "rightAmount"),
    detail::bipolar("talas.limeInterpreter", "left"),
    detail::bipolar("talas.limeInterpreter", "right"),
    detail::frequency("talas.monotonizer", "frequency", 0.5),
    detail::frequency("talas.monotonizer", "maxInputPitch", 0.5),
    detail::frequency("talas.monotonizer", "minInputPitch", 0.5),
    detail::nonZeroRate("talas.multifileDiskPlayer", "rateScale"),
    // Which file plays: a whole index, rounded as it moves (an index below
    // the first selects the first).
    detail::whole("talas.multifileDiskPlayer", "index", -detail::unbounded, detail::unbounded),
    detail::whole("talas.multisample", "index", 0.0, detail::unbounded),
    detail::whole("talas.timeIndexForFileNames", "index", 0.0, detail::unbounded),
    detail::whole("talas.timeIndexForSamples", "index", 0.0, detail::unbounded),
    detail::unit("talas.multisample", "loopEnd"),
    detail::unit("talas.multisample", "loopStart"),
    // An AnalogSequencer's levels within -1 to 1 and its Rate never zero
    //.
    detail::bipolar("talas.analogSequencer", "left"),
    detail::nonZeroRate("talas.analogSequencer", "rate"),
    detail::bipolar("talas.analogSequencer", "right"),
    detail::time("talas.adsr", "attackTime"),
    detail::time("talas.adsr", "decayTime"),
    detail::time("talas.adsr", "releaseTime"),
    detail::time("talas.amplitudeFollower", "timeConstant"),
    detail::unit("talas.antialiasedOscillator", "brightness"),
    detail::closed("talas.antialiasedOscillator", "dutyCycle", 0.01, 0.99),
    detail::time("talas.ar", "attackTime"),
    detail::time("talas.ar", "releaseTime"),
    detail::time("talas.chopper", "grainDuration"),
    detail::time("talas.chopper", "interGrainDelay"),
    // A Chopper timed by beats: a Rate above zero
    // and a DutyCycle that neither stays shut nor stays open.
    detail::frequency("talas.chopper", "rate"),
    Range{"talas.chopper", "dutyCycle", Unit::Scalar, 0.0, true, 1.0, true},
    detail::whole("talas.clockDivider", "division", 1.0, 4294967295.0),
    detail::whole("talas.clockDivider", "steps", 1.0, 4294967295.0),
    detail::frequency("talas.clock", "frequency"),
    detail::unit("talas.delayWithFeedback", "delayScale"),
    detail::unit("talas.diffuseSpectralStretch", "diffusion"),
    detail::whole("talas.diffuseSpectralStretch", "seed", 0.0, 4294967295.0),
    detail::unit("talas.diffuseSpectralStretch", "smear"),
    detail::closed("talas.diffuseSpectralStretch", "stretch", 2.0, 32.0),
    detail::time("talas.diskPlayer", "duration"),
    detail::nonZeroRate("talas.diskPlayer", "rateScale"),
    detail::closed("talas.dualMorphOscillator", "ratio", 0.125, 16.0),
    detail::closed("talas.dynamicBreakpointOscillator", "frequency", 0.1, detail::unbounded, Unit::Hertz),
    detail::whole("talas.dynamicBreakpointOscillator", "pointCount", 4.0, 64.0),
    detail::whole("talas.dynamicBreakpointOscillator", "seed", 1.0, 4294967295.0),
    detail::time("talas.dynamicRangeController", "attackTime"),
    detail::time("talas.dynamicRangeController", "releaseTime"),
    // A Ratio above zero, then within what the Type takes.
    detail::positive("talas.dynamicRangeController", "ratio"),
    detail::frequency("talas.euSeq", "baseFrequency", 0.25),
    detail::whole("talas.euSeq", "pulses", 0.0, 256.0),
    detail::whole("talas.euSeq", "rotation", 0.0, 255.0),
    detail::whole("talas.euSeq", "scaleSteps", 1.0, 96.0),
    detail::whole("talas.euSeq", "steps", 1.0, 256.0),
    detail::whole("talas.euSeq", "stepsPerOctave", 1.0, 96.0),
    detail::whole("talas.filter", "order", 1.0, 16.0),
    detail::positive("talas.filter", "q"),
    detail::closed("talas.frequencyScale", "frequencyScale", 1.0 / 64.0, 64.0),
    detail::time("talas.functionGenerator", "onDuration"),
    // A fundamental above zero.
    detail::frequency("talas.formantBank", "frequency"),
    detail::frequency("talas.formantBankOscillator", "frequency"),
    detail::whole("talas.grainCloud", "maxGrains", 1.0, 64.0),
    detail::frequency("talas.gaOscillators", "frequency", 0.5),
    // From the envelopes' beginning at -1 to their end at 1.
    detail::bipolar("talas.gaOscillators", "timeIndex"),
    detail::unit("talas.graphicalEnvelope", "level"),
    detail::positive("talas.graphicalEnvelope", "rate"),
    detail::time("talas.harmonicResonator", "decayTime"),
    // Its period must fit the delay line a realtime renderer holds, 480000
    // frames by default (10 s at 48 kHz).
    Range{"talas.harmonicResonator", "frequency", Unit::Hertz, 0.0, false, detail::unbounded, false, 0.0,
        1.0 / 480000.0},
    detail::frequency("talas.highShelvingFilter", "cutoffFreq", 0.5),
    detail::whole("talas.iteratedWaveshaper", "iterations", 1.0, 4096.0),
    detail::time("talas.linearRamp", "duration"),
    detail::closed("talas.liveSpectralAnalysis", "ampScale", 0.0, detail::unbounded),
    detail::positive("talas.liveSpectralAnalysis", "freqScale"),
    detail::frequency("talas.lowShelvingFilter", "cutoffFreq", 0.5),
    // A Low-Pass Gate's setup, read as each voice starts.
    detail::closed("talas.lowPassGate", "attackTime", 0.0, detail::unbounded, Unit::Seconds),
    detail::frequency("talas.lowPassGate", "closedFrequency", 0.5),
    detail::closed("talas.lowPassGate", "curve", 0.25, 8.0),
    detail::frequency("talas.lowPassGate", "openFrequency", 0.5),
    detail::closed("talas.lowPassGate", "releaseTime", 0.0, detail::unbounded, Unit::Seconds),
    detail::bipolar("talas.midiMapper", "left"),
    detail::bipolar("talas.midiMapper", "right"),
    detail::bipolar("talas.midiVoice", "left"),
    detail::bipolar("talas.midiVoice", "right"),
    detail::bipolar("talas.modalFilter", "envelope"),
    detail::positive("talas.modalFilter", "frequencyScale", 16.0),
    detail::bipolar("talas.modalFilter", "inputLevel"),
    detail::frequency("talas.morphFilter", "cutoff", 0.45),
    detail::positive("talas.multiplyingWaveshaper", "scale"),
    detail::unit("talas.multisegmentEnvelope", "level"),
    detail::positive("talas.multislopeFunctionGenerator", "rate"),
    detail::bipolar("talas.noise", "initialState"),
    detail::closed("talas.oscillator", "maxMI", 0.0, detail::unbounded),
    detail::bipolar("talas.overlappingMixer", "left"),
    detail::bipolar("talas.overlappingMixer", "right"),
    detail::time("talas.peakDetector", "attackTime"),
    detail::time("talas.peakDetector", "releaseTime"),
    detail::frequency("talas.phaseShiftBy90", "frequency", 0.5),
    detail::closed("talas.pluckedWaveguide", "decayTime", 0.02, 20.0, Unit::Seconds),
    Range{"talas.pluckedWaveguide", "frequency", Unit::Hertz, 20.0, false, detail::unbounded, false, 0.4},
    detail::closed("talas.pluckedWaveguide", "pickPosition", 0.02, 0.98),
    detail::whole("talas.pluckedWaveguide", "seed", 1.0, 4294967295.0),
    detail::frequency("talas.presenceFilter", "bandwidth"),
    detail::whole("talas.probabilityChaosGenerator", "seed", 1.0, 4294967295.0),
    detail::frequency("talas.presenceFilter", "centerFreq", 0.5),
    detail::time("talas.pulseTrain", "period"),
    detail::closed("talas.quadFunctionGenerator", "curve", 0.25, 4.0),
    detail::closed("talas.quadFunctionGenerator", "skew", 0.05, 0.95),
    detail::positive("talas.quantizer", "stepSize"),
    detail::time("talas.ramp", "duration"),
    detail::time("talas.reverbSection", "decayTime"),
    detail::unit("talas.reverbSection", "delayScale"),
    detail::time("talas.rmsSquared", "timeConstant"),
    detail::unit("talas.sample", "end"),
    detail::whole("talas.scannedMassOscillator", "massCount", 8.0, 64.0),
    detail::unit("talas.scannedMassOscillator", "initialShape"),
    detail::unit("talas.sample", "loopEnd"),
    detail::time("talas.sampleAndHold", "holdTime"),
    detail::time("talas.sampleCloud", "grainDur"),
    detail::whole("talas.sampleCloud", "maxGrains", 1.0, 64.0),
    detail::whole("talas.samplesFromDiskSingleStep", "filePosition", 0.0, detail::unbounded),
    detail::positive("talas.saturator", "transitionWidth", 1.0),
    detail::whole("talas.scaleVocoder", "bankSize", 1.0, 4096.0),
    // A bank holds up to 64 bands.
    detail::whole("talas.scaleVocoder", "nbrBands", 1.0, 64.0),
    detail::inheritingTime("talas.scaleVocoder", "attackTime"),
    detail::positive("talas.scaleVocoder", "inBandwidth"),
    detail::inheritingTime("talas.scaleVocoder", "releaseTime"),
    detail::positive("talas.scaleVocoder", "sideBandwidth"),
    detail::bipolar("talas.script", "left"),
    detail::bipolar("talas.script", "right"),
    detail::time("talas.setDuration", "duration"),
    detail::frequency("talas.simplePitchShifter", "maxInputPitch"),
    detail::frequency("talas.simplePitchShifter", "minInputPitch"),
    detail::positive("talas.singleSideBandRm", "freqScale"),
    detail::frequency("talas.singleSideBandRm", "frequency", 0.5),
    // A SpectrumModifier's criteria and changes: whole track
    // numbers, frequencies, hysteresis and scales from zero, a probability
    // from zero (clipped to 1 as it plays), smoothing below 1.
    detail::closed("talas.spectrumModifier", "ampHysteresis", 0.0, detail::unbounded),
    Range{"talas.spectrumModifier", "ampSmoothing", Unit::Scalar, 0.0, false, 1.0, true},
    detail::closed("talas.spectrumModifier", "freqHysteresis", 0.0, detail::unbounded, Unit::Hertz),
    detail::closed("talas.spectrumModifier", "freqScale", 0.0, detail::unbounded),
    Range{"talas.spectrumModifier", "freqSmoothing", Unit::Scalar, 0.0, false, 1.0, true},
    detail::closed("talas.spectrumModifier", "hiFreq", 0.0, detail::unbounded, Unit::Hertz),
    detail::whole("talas.spectrumModifier", "hiTrack", 1.0, 9007199254740991.0),
    detail::closed("talas.spectrumModifier", "loFreq", 0.0, detail::unbounded, Unit::Hertz),
    detail::whole("talas.spectrumModifier", "loTrack", 1.0, 9007199254740991.0),
    detail::closed("talas.spectrumModifier", "probability", 0.0, detail::unbounded),
    detail::closed("talas.stereoDispersiveResonator", "decayTime", 0.01, 20.0, Unit::Seconds),
    // Its branches, a Detune of up to 100 cents (2^(100/2400) =
    // 1.029302236643492) either side, take 20 Hz to 0.45 of the sample
    // rate, so the Frequency stays where any Detune keeps them there.
    Range{"talas.stereoDispersiveResonator", "frequency", Unit::Hertz, 20.58604473286984, false, detail::unbounded,
        false, 0.4371893735191226},
    detail::unit("talas.stereoDispersiveResonator", "mix"),
    detail::unit("talas.stereoDispersiveResonator", "width"),
    detail::bipolar("talas.stereoMix2", "left"),
    detail::unit("talas.stereoMix2", "pan1"),
    detail::unit("talas.stereoMix2", "pan2"),
    detail::bipolar("talas.stereoMix2", "right"),
    detail::unit("talas.stereoMix2", "scale1"),
    detail::unit("talas.stereoMix2", "scale2"),
    detail::bipolar("talas.stereoMix4", "left"),
    detail::unit("talas.stereoMix4", "pan1"),
    detail::unit("talas.stereoMix4", "pan2"),
    detail::unit("talas.stereoMix4", "pan3"),
    detail::unit("talas.stereoMix4", "pan4"),
    detail::bipolar("talas.stereoMix4", "right"),
    detail::unit("talas.stereoMix4", "scale1"),
    detail::unit("talas.stereoMix4", "scale2"),
    detail::unit("talas.stereoMix4", "scale3"),
    detail::unit("talas.stereoMix4", "scale4"),
    detail::frequency("talas.sumOfSines", "analysisFrequency0", 0.5),
    detail::frequency("talas.sumOfSines", "analysisFrequency1", 0.5),
    detail::whole("talas.sumOfSines", "analysisFrames0", 1.0, 4096.0),
    detail::whole("talas.sumOfSines", "analysisFrames1", 1.0, 4096.0),
    detail::positive("talas.sumOfSines", "bankSize"),
    detail::whole("talas.sumOfSines", "nbrPartials", 1.0, 4096.0),
    detail::time("talas.sumOfSines", "onDuration"),
    detail::whole("talas.syntheticSpectrumFromArray", "nbrPartials", 1.0, 4096.0),
    detail::positive("talas.timeFrequencyScale", "frequencyScale"),
    detail::frequency("talas.timeFrequencyScale", "maxInputFreq", 0.5),
    detail::positive("talas.timeFrequencyScale", "rate"),
    detail::time("talas.timeIndex", "duration"),
    detail::positive("talas.tunableVocoder", "analysisBW"),
    detail::inheritingTime("talas.tunableVocoder", "attackTime"),
    detail::inheritingTime("talas.tunableVocoder", "releaseTime"),
    detail::whole("talas.tunableVocoder", "bankSize", 1.0, 4096.0),
    detail::whole("talas.tunableVocoder", "nbrBands", 1.0, 64.0),
    detail::frequency("talas.tunableVocoder", "analysisFreq"),
    detail::positive("talas.tunableVocoder", "analysisSpacing"),
    detail::positive("talas.tunableVocoder", "synthesisBW"),
    detail::frequency("talas.tunableVocoder", "synthesisFreq"),
    detail::positive("talas.tunableVocoder", "synthesisSpacing"),
    detail::frequency("talas.twoFormantElement", "bandwidth1"),
    detail::frequency("talas.twoFormantElement", "bandwidth2"),
    detail::frequency("talas.twoFormantElement", "formant1", 0.5),
    detail::frequency("talas.twoFormantElement", "formant2", 0.5),
    detail::frequency("talas.twoFormantVoiceElement", "bandwidth1"),
    detail::frequency("talas.twoFormantVoiceElement", "bandwidth2"),
    detail::frequency("talas.twoFormantVoiceElement", "formant1", 0.5),
    detail::frequency("talas.twoFormantVoiceElement", "formant2", 0.5),
    detail::frequency("talas.twoFormantVoiceElement", "frequency", 0.5),
    detail::frequency("talas.vcf", "cutoff", 0.5),
    detail::inheritingTime("talas.vocoder", "attackTime"),
    // A WaveTerrainOscillator's orbit stays on its terrain.
    detail::unit("talas.waveTerrainOscillator", "xRadius"),
    detail::unit("talas.waveTerrainOscillator", "yRadius"),
    detail::closed("talas.waveTerrainOscillator", "yRatio", 0.25, 8.0),
    detail::whole("talas.vocoder", "bankSize", 1.0, 4096.0),
    detail::whole("talas.vocoder", "nbrBands", 1.0, 64.0),
    detail::inheritingTime("talas.vocoder", "releaseTime"),
    detail::inheritingTime("talas.vocoderChannelBank", "attackTime"),
    detail::inheritingTime("talas.vocoderChannelBank", "releaseTime"),
    detail::positive("talas.vocoder", "bw"),
    detail::positive("talas.vocoder", "inFreq"),
    detail::frequency("talas.vocoder", "lowCF"),
    detail::frequency("talas.vocoder", "highCF"),
    detail::frequency("talas.vocoder", "hiCutoff"),
    detail::positive("talas.vocoder", "sideFreq"),
};

[[nodiscard]] constexpr const Range* find(std::string_view typeId, std::string_view parameterId) noexcept
{
    const auto found = std::ranges::find_if(ranges, [&](const Range& range) {
        return range.typeId == typeId && range.parameterId == parameterId;
    });
    return found == ranges.end() ? nullptr : &*found;
}

// One step inside an open edge.
[[nodiscard]] inline double quantum(Unit unit, double sampleRate) noexcept
{
    constexpr double fixedPointStep = 1.0 / 16777216.0;
    switch (unit)
    {
    case Unit::Seconds:
        return 1.0 / sampleRate;
    case Unit::Hertz:
        return sampleRate * fixedPointStep;
    case Unit::Scalar:
        break;
    }
    return 2.0 * fixedPointStep;
}

// The value, or the edge it passed, rounded when the field is whole; a value
// that is not finite is left unchanged.
[[nodiscard]] inline double saturate(const Range& range, double value, double sampleRate) noexcept
{
    if (!std::isfinite(value) || !std::isfinite(sampleRate) || sampleRate <= 0.0
        || (range.inheritsAtMinusOne && value == -1.0))
        return value;
    const auto step = quantum(range.unit, sampleRate);
    if (range.nonZero && value == 0.0)
        return step;
    const auto high = range.highPerSampleRate > 0.0 ? range.highPerSampleRate * sampleRate : range.high;
    const auto low = range.lowPerSampleRate > 0.0 ? range.lowPerSampleRate * sampleRate : range.low;
    if (range.lowOpen ? value <= low : value < low)
        value = range.lowOpen ? low + step : low;
    else if (range.highOpen ? value >= high : value > high)
        value = range.highOpen ? high - step : high;
    return range.whole ? std::round(value) : value;
}

[[nodiscard]] inline double saturate(
    std::string_view typeId, std::string_view parameterId, double value, double sampleRate) noexcept
{
    const auto* range = find(typeId, parameterId);
    return range == nullptr ? value : saturate(*range, value, sampleRate);
}

// A project's choice (format 156): the inverse of 0
// is infinity, which a live field takes as its largest legal value, or for
// minus infinity its smallest. A field without that edge, or a value that
// is finite or undefined, is left as it is, so that it is still refused.
[[nodiscard]] inline double saturateInfinity(const Range* range, double value, double sampleRate) noexcept
{
    if (range == nullptr || !std::isinf(value))
        return value;
    const auto high = range->highPerSampleRate > 0.0 ? range->highPerSampleRate * sampleRate : range->high;
    const auto low = range->lowPerSampleRate > 0.0 ? range->lowPerSampleRate * sampleRate : range->low;
    if ((value > 0.0 && !std::isfinite(high)) || (value < 0.0 && !std::isfinite(low)))
        return value;
    const auto largest = std::numeric_limits<double>::max();
    return saturate(*range, value > 0.0 ? largest : -largest, sampleRate);
}

[[nodiscard]] inline double saturateInfinity(
    std::string_view typeId, std::string_view parameterId, double value, double sampleRate) noexcept
{
    return saturateInfinity(find(typeId, parameterId), value, sampleRate);
}
}
