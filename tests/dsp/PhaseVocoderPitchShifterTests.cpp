#include "talas/dsp/PhaseVocoderPitchShifter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace
{
std::atomic<bool> countAllocations = false;
std::atomic<std::size_t> allocationCount = 0;
}

void* operator new(std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    if (countAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size))
        return memory;
    throw std::bad_alloc{};
}

namespace
{
static_assert(talas::dsp::phaseVocoderTransientFluxThreshold == 0.35);
static_assert(talas::dsp::phaseVocoderTransientEnergyFloor == 1.0e-12);
static_assert(talas::dsp::phaseVocoderTransientRefractoryHops == 3);
static_assert(talas::dsp::phaseVocoderIdentityPeakRelativeFloor == 0.05);
static_assert(
    talas::dsp::phaseVocoderPeakIdentityMaximumDisplacementBins == 4);
static_assert(talas::dsp::phaseVocoderSpectralEnvelopeRadiusBins == 8);

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
inline constexpr bool sanitizerInstrumented = true;
#else
inline constexpr bool sanitizerInstrumented = false;
#endif

// The callback budgets are wall-clock times, which hold only while nothing
// else loads the machine, so they are checked only when TALAS_STRICT_TIMING=1:
// ctest's serial "perf" entry for this binary sets it, and the local gate
// runs that label after the parallel suite. Other runs print the timings.
bool strictTiming()
{
    const char* value = std::getenv("TALAS_STRICT_TIMING");
    return value != nullptr && std::string_view{value} == "1";
}

void require(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::vector<double> makeTone(
    double sampleRate,
    double frequency,
    std::size_t sourceFrames)
{
    std::vector<double> input(
        sourceFrames + 2 * talas::dsp::phaseVocoderPitchFrameSize,
        0.0);
    for (std::size_t frame = 0; frame < sourceFrames; ++frame)
    {
        input[frame] = 0.25 * std::sin(
            2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate);
    }
    return input;
}

std::vector<double> makeHarmonicStack(
    double sampleRate,
    double fundamental,
    std::size_t sourceFrames)
{
    std::vector<double> input(
        sourceFrames + 2 * talas::dsp::phaseVocoderPitchFrameSize,
        0.0);
    for (std::size_t frame = 0; frame < sourceFrames; ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi * fundamental
            * static_cast<double>(frame) / sampleRate;
        input[frame] = 0.12 * std::sin(phase)
            + 0.08 * std::sin(2.0 * phase)
            + 0.06 * std::sin(3.0 * phase)
            + 0.04 * std::sin(4.0 * phase);
    }
    return input;
}

std::vector<double> makeHarmonicGlide(
    double sampleRate,
    double startFundamental,
    double endFundamental,
    std::size_t sourceFrames)
{
    std::vector<double> input(
        sourceFrames + 2 * talas::dsp::phaseVocoderPitchFrameSize,
        0.0);
    double phase = 0.0;
    for (std::size_t frame = 0; frame < sourceFrames; ++frame)
    {
        const auto fraction = sourceFrames > 1
            ? static_cast<double>(frame)
                / static_cast<double>(sourceFrames - 1)
            : 0.0;
        const auto fundamental = startFundamental
            + fraction * (endFundamental - startFundamental);
        phase += 2.0 * std::numbers::pi * fundamental / sampleRate;
        input[frame] = 0.12 * std::sin(phase)
            + 0.08 * std::sin(2.0 * phase)
            + 0.06 * std::sin(3.0 * phase)
            + 0.04 * std::sin(4.0 * phase);
    }
    return input;
}

std::vector<double> makeFormantShapedHarmonicStack(
    double sampleRate,
    double fundamental,
    std::size_t sourceFrames)
{
    std::vector<double> input(
        sourceFrames + 2 * talas::dsp::phaseVocoderPitchFrameSize,
        0.0);
    for (std::size_t frame = 0; frame < sourceFrames; ++frame)
    {
        const auto basePhase = 2.0 * std::numbers::pi * fundamental
            * static_cast<double>(frame) / sampleRate;
        double sample = 0.0;
        for (std::size_t harmonic = 1; harmonic <= 24; ++harmonic)
        {
            const auto frequency = fundamental
                * static_cast<double>(harmonic);
            const auto primaryDistance = (frequency - 1500.0) / 300.0;
            const auto secondaryDistance = (frequency - 3000.0) / 450.0;
            const auto amplitude = 0.004
                + 0.055 * std::exp(-0.5 * primaryDistance * primaryDistance)
                + 0.025 * std::exp(
                    -0.5 * secondaryDistance * secondaryDistance);
            sample += amplitude * std::sin(
                static_cast<double>(harmonic) * basePhase);
        }
        input[frame] = sample;
    }
    return input;
}

std::vector<double> render(
    double sampleRate,
    double interval,
    std::span<const double> input,
    const std::vector<std::size_t>& partitions,
    talas::dsp::PhaseVocoderPitchProfile profile = talas::dsp::PhaseVocoderPitchProfile::OneOctave)
{
    const auto maximum = *std::ranges::max_element(partitions);
    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        sampleRate,
        maximum,
        interval,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
        profile,
    };
    std::vector<double> output(input.size(), 0.0);
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < input.size())
    {
        const auto count = std::min(
            partitions[partition % partitions.size()],
            input.size() - offset);
        require(
            processor.process(
                input.subspan(offset, count),
                {7, 3, offset},
                0,
                std::span<double>{output}.subspan(offset, count))
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "phase-vocoder block should process");
        offset += count;
        ++partition;
    }
    return output;
}

std::vector<double> renderWithEnvelopePolicy(
    double sampleRate,
    double interval,
    std::span<const double> input,
    const std::vector<std::size_t>& partitions,
    talas::dsp::PhaseVocoderSpectralEnvelopePolicy policy)
{
    const auto maximum = *std::ranges::max_element(partitions);
    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        sampleRate,
        maximum,
        interval,
        policy,
    };
    require(processor.spectralEnvelopePolicy() == policy,
            "prepared vocoder should retain its fixed envelope policy");
    std::vector<double> output(input.size(), 0.0);
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < input.size())
    {
        const auto count = std::min(
            partitions[partition % partitions.size()],
            input.size() - offset);
        require(
            processor.process(
                input.subspan(offset, count),
                {96, 3, offset},
                0,
                std::span<double>{output}.subspan(offset, count))
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "spectral-envelope policy fixture should process");
        offset += count;
        ++partition;
    }
    return output;
}

double projectedAmplitude(
    std::span<const double> signal,
    double sampleRate,
    double frequency)
{
    double sine = 0.0;
    double cosine = 0.0;
    for (std::size_t frame = 0; frame < signal.size(); ++frame)
    {
        const auto phase = 2.0 * std::numbers::pi * frequency
            * static_cast<double>(frame) / sampleRate;
        sine += signal[frame] * std::sin(phase);
        cosine += signal[frame] * std::cos(phase);
    }
    return 2.0 * std::hypot(sine, cosine)
        / static_cast<double>(signal.size());
}

double strongestShiftedHarmonicFrequency(
    std::span<const double> signal,
    double sampleRate,
    double shiftedFundamental)
{
    double strongestFrequency = 0.0;
    double strongestAmplitude = -1.0;
    for (std::size_t harmonic = 1; harmonic <= 24; ++harmonic)
    {
        const auto frequency = shiftedFundamental
            * static_cast<double>(harmonic);
        if (frequency >= sampleRate * 0.5)
            break;
        const auto amplitude = projectedAmplitude(
            signal, sampleRate, frequency);
        if (amplitude > strongestAmplitude)
        {
            strongestAmplitude = amplitude;
            strongestFrequency = frequency;
        }
    }
    return strongestFrequency;
}

double shiftedHarmonicCentroid(
    std::span<const double> signal,
    double sampleRate,
    double shiftedFundamental)
{
    double weightedFrequency = 0.0;
    double totalAmplitude = 0.0;
    for (std::size_t harmonic = 1; harmonic <= 24; ++harmonic)
    {
        const auto frequency = shiftedFundamental
            * static_cast<double>(harmonic);
        if (frequency >= sampleRate * 0.5)
            break;
        const auto amplitude = projectedAmplitude(
            signal, sampleRate, frequency);
        weightedFrequency += frequency * amplitude;
        totalAmplitude += amplitude;
    }
    return weightedFrequency / totalAmplitude;
}

std::vector<double> renderDynamic(
    double sampleRate,
    std::span<const double> input,
    const std::array<std::size_t, 5>& sectionStarts,
    const std::array<double, 4>& intervals,
    const std::vector<std::size_t>& partitions,
    talas::dsp::PhaseVocoderPitchProfile profile = talas::dsp::PhaseVocoderPitchProfile::OneOctave)
{
    const auto maximum = *std::ranges::max_element(partitions);
    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter processor{
        sampleRate,
        maximum,
        intervals.front(),
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
        profile,
    };
    std::vector<double> output(input.size(), 0.0);
    std::size_t offset = 0;
    std::size_t partition = 0;
    std::size_t section = 0;
    while (offset < input.size())
    {
        while (section + 1 < intervals.size()
               && offset >= sectionStarts[section + 1])
        {
            ++section;
        }
        const auto count = std::min({
            partitions[partition % partitions.size()],
            input.size() - offset,
            sectionStarts[section + 1] - offset,
        });
        require(
            processor.process(
                input.subspan(offset, count),
                intervals[section],
                {17, 2, offset},
                0,
                std::span<double>{output}.subspan(offset, count))
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "dynamic phase-vocoder block should process");
        offset += count;
        ++partition;
    }
    return output;
}

double estimateFrequency(
    std::span<const double> signal,
    double sampleRate,
    double expected)
{
    const auto minimumLag = static_cast<std::size_t>(
        std::floor(sampleRate / (expected * 1.08)));
    const auto maximumLag = static_cast<std::size_t>(
        std::ceil(sampleRate / (expected * 0.92)));
    double bestScore = -1.0;
    std::size_t bestLag = minimumLag;
    for (std::size_t lag = minimumLag; lag <= maximumLag; ++lag)
    {
        double score = 0.0;
        double energyA = 0.0;
        double energyB = 0.0;
        for (std::size_t index = lag; index < signal.size(); ++index)
        {
            score += signal[index] * signal[index - lag];
            energyA += signal[index] * signal[index];
            energyB += signal[index - lag] * signal[index - lag];
        }
        const auto normalized = score / std::sqrt(energyA * energyB);
        if (normalized > bestScore)
        {
            bestScore = normalized;
            bestLag = lag;
        }
    }
    if (bestScore <= 0.8)
    {
        std::cerr << "periodicity expected=" << expected
                  << " bestScore=" << bestScore
                  << " bestLag=" << bestLag << '\n';
    }
    require(bestScore > 0.8, "shifted tone should retain a periodic waveform");
    return sampleRate / static_cast<double>(bestLag);
}

void requireContinuousHopBoundaries(
    std::span<const double> signal,
    std::size_t firstFrame,
    std::size_t frameCount)
{
    const auto inspected = signal.subspan(firstFrame, frameCount);
    double squaredDeltaSum = 0.0;
    std::size_t deltaCount = 0;
    double maximumHopBoundaryDelta = 0.0;
    for (std::size_t frame = 1; frame < inspected.size(); ++frame)
    {
        const auto delta = std::abs(inspected[frame] - inspected[frame - 1]);
        squaredDeltaSum += delta * delta;
        ++deltaCount;
        if ((firstFrame + frame) % talas::dsp::phaseVocoderPitchHopSize == 0)
            maximumHopBoundaryDelta = std::max(maximumHopBoundaryDelta, delta);
    }
    const auto rmsDelta = std::sqrt(
        squaredDeltaSum / static_cast<double>(deltaCount));
    require(rmsDelta > 0.0,
            "shifted tone should retain non-zero phase motion");
    require(maximumHopBoundaryDelta < 4.0 * rmsDelta,
            "analysis-hop boundaries should not introduce phase jumps");
}

struct TransientRenderResult
{
    std::vector<double> output;
    std::size_t phaseAnchors = 0;
    std::size_t identityLockedFrames = 0;
    std::size_t matchedPeakIdentities = 0;
    std::size_t activePeakIdentities = 0;
};

TransientRenderResult renderTransientFixture(
    std::span<const double> input,
    const std::vector<std::size_t>& partitions,
    double interval = 7.0)
{
    const auto maximum = *std::ranges::max_element(partitions);
    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        48000.0,
        maximum,
        interval,
    };
    TransientRenderResult result{
        std::vector<double>(input.size(), 0.0),
        0,
        0,
        0,
        0,
    };
    std::size_t offset = 0;
    std::size_t partition = 0;
    while (offset < input.size())
    {
        const auto count = std::min(
            partitions[partition % partitions.size()],
            input.size() - offset);
        require(
            processor.process(
                input.subspan(offset, count),
                {91, 4, offset},
                0,
                std::span<double>{result.output}.subspan(offset, count))
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "transient fixture should process");
        offset += count;
        ++partition;
    }
    result.phaseAnchors = processor.transientPhaseAnchorCount();
    result.identityLockedFrames = processor.identityLockedFrameCount();
    result.matchedPeakIdentities = processor.peakIdentityMatchCount();
    result.activePeakIdentities = processor.activePeakIdentityCount();
    return result;
}

TransientRenderResult renderIntervalSections(
    std::span<const double> input,
    const std::array<std::size_t, 5>& sectionStarts,
    const std::array<double, 4>& intervals,
    const std::vector<std::size_t>& partitions,
    talas::dsp::PhaseVocoderSpectralEnvelopePolicy policy =
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum)
{
    const auto maximum = *std::ranges::max_element(partitions);
    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        48000.0,
        maximum,
        intervals.front(),
        policy,
    };
    TransientRenderResult result{
        std::vector<double>(input.size(), 0.0),
        0,
        0,
        0,
        0,
    };
    std::size_t offset = 0;
    std::size_t partition = 0;
    std::size_t section = 0;
    while (offset < input.size())
    {
        while (section + 1 < intervals.size()
               && offset >= sectionStarts[section + 1])
        {
            ++section;
        }
        const auto sectionEnd = section + 1 < sectionStarts.size()
            ? sectionStarts[section + 1]
            : input.size();
        const auto count = std::min({
            partitions[partition % partitions.size()],
            input.size() - offset,
            sectionEnd - offset,
        });
        require(
            processor.processWithInterval(
                input.subspan(offset, count),
                intervals[section],
                {94, 6, offset},
                0,
                std::span<double>{result.output}.subspan(offset, count))
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "cross-frame ratio-ramp fixture should process");
        offset += count;
        ++partition;
    }
    result.phaseAnchors = processor.transientPhaseAnchorCount();
    result.identityLockedFrames = processor.identityLockedFrameCount();
    result.matchedPeakIdentities = processor.peakIdentityMatchCount();
    result.activePeakIdentities = processor.activePeakIdentityCount();
    return result;
}

void testIdentityPhaseLockingPreservesHarmonicCoherence()
{
    constexpr double sampleRate = 48000.0;
    constexpr double fundamental = 187.5;
    constexpr double ratio = 1.5;
    constexpr std::size_t sourceFrames = 32768;
    const auto interval = talas::dsp::phaseVocoderFrequencyScaleInterval(ratio);
    const auto inputStorage = makeHarmonicStack(
        sampleRate, fundamental, sourceFrames);
    const auto input = std::span<const double>{inputStorage};
    const auto regular = renderTransientFixture(input, {256}, interval);
    const auto irregular = renderTransientFixture(
        input, {3, 509, 64, 1, 257, 17}, interval);

    require(regular.output == irregular.output,
            "identity phase locking should be callback-partition invariant");
    require(regular.identityLockedFrames
                == irregular.identityLockedFrames,
            "identity-lock decisions should be callback-partition invariant");
    require(regular.identityLockedFrames > 32,
            "steady harmonic material should exercise bounded multi-peak locking");
    require(regular.matchedPeakIdentities
                == irregular.matchedPeakIdentities,
            "cross-frame peak matches should be callback-partition invariant");
    require(regular.matchedPeakIdentities > 64,
            "steady harmonic material should reuse bounded peak identities");
    require(regular.activePeakIdentities == 0,
            "the padded silent tail should expire all peak identities");

    const auto analysisStart =
        talas::dsp::phaseVocoderPitchLatencyFrames + 8192;
    const auto measured = estimateFrequency(
        std::span<const double>{regular.output}.subspan(
            analysisStart, 16384),
        sampleRate,
        fundamental * ratio);
    require(std::abs(measured - fundamental * ratio)
                / (fundamental * ratio) < 0.015,
            "identity-locked harmonic stack should retain the requested fundamental ratio");
    requireContinuousHopBoundaries(
        regular.output, analysisStart, 16384);
}

void testCrossFramePeakIdentityTracksGlideAndRatioRamp()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t sourceFrames = 32768;
    constexpr std::size_t totalFrames = sourceFrames
        + 2 * talas::dsp::phaseVocoderPitchFrameSize;
    const auto glideStorage = makeHarmonicGlide(
        sampleRate, 170.0, 230.0, sourceFrames);
    const auto glide = std::span<const double>{glideStorage};
    const auto regular = renderTransientFixture(glide, {256}, 7.0);
    const auto irregular = renderTransientFixture(
        glide, {1, 509, 17, 64, 3, 257}, 7.0);
    require(regular.output == irregular.output,
            "bin-crossing harmonic glide should be partition invariant");
    require(regular.matchedPeakIdentities
                == irregular.matchedPeakIdentities,
            "bin-crossing peak ownership should be partition invariant");
    require(regular.matchedPeakIdentities > 64,
            "slow harmonic motion should retain nearby peak identities");
    requireContinuousHopBoundaries(
        regular.output,
        talas::dsp::phaseVocoderPitchLatencyFrames + 8192,
        16384);

    constexpr std::array<std::size_t, 5> sectionStarts{
        0, 8192, 16384, 24576, totalFrames};
    constexpr std::array<double, 4> intervals{3.0, 7.0, -2.0, 5.0};
    const auto rampRegular = renderIntervalSections(
        glide, sectionStarts, intervals, {256});
    const auto rampIrregular = renderIntervalSections(
        glide, sectionStarts, intervals, {3, 509, 1, 257, 64, 17});
    require(rampRegular.output == rampIrregular.output,
            "four-hop ratio ramps should remain partition invariant");
    require(rampRegular.matchedPeakIdentities
                == rampIrregular.matchedPeakIdentities,
            "ratio-ramp peak matching should remain partition invariant");
    require(rampRegular.matchedPeakIdentities > 32,
            "ratio motion should exercise cross-frame peak matching");
}

void testFixedSpectralEnvelopePolicyPreservesBroadFormants()
{
    constexpr double sampleRate = 48000.0;
    constexpr double fundamental = 187.5;
    constexpr double ratio = 1.5;
    constexpr std::size_t sourceFrames = 32768;
    const auto interval = talas::dsp::phaseVocoderFrequencyScaleInterval(ratio);
    const auto inputStorage = makeFormantShapedHarmonicStack(
        sampleRate, fundamental, sourceFrames);
    const auto input = std::span<const double>{inputStorage};
    const auto defaultScale = render(
        sampleRate, interval, input, {256});
    const auto explicitScale = renderWithEnvelopePolicy(
        sampleRate,
        interval,
        input,
        {256},
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum);
    require(defaultScale == explicitScale,
            "explicit scale policy should preserve the accepted Step 8 output");

    const auto preserveRegular = renderWithEnvelopePolicy(
        sampleRate,
        interval,
        input,
        {256},
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::PreserveAbsoluteFrequency);
    const auto preserveIrregular = renderWithEnvelopePolicy(
        sampleRate,
        interval,
        input,
        {3, 509, 1, 257, 64, 17},
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::PreserveAbsoluteFrequency);
    require(preserveRegular == preserveIrregular,
            "fixed formant preservation should be partition invariant");
    require(preserveRegular != explicitScale,
            "preserve and scale policies should remain observably distinct");

    const auto analysisStart =
        talas::dsp::phaseVocoderPitchLatencyFrames + 8192;
    const auto scaleStrongest = strongestShiftedHarmonicFrequency(
        std::span<const double>{explicitScale}.subspan(
            analysisStart, 16384),
        sampleRate,
        fundamental * ratio);
    const auto sourceCentroid = shiftedHarmonicCentroid(
        input.subspan(8192, 16384), sampleRate, fundamental);
    const auto scaleCentroid = shiftedHarmonicCentroid(
        std::span<const double>{explicitScale}.subspan(
            analysisStart, 16384),
        sampleRate,
        fundamental * ratio);
    const auto preserveCentroid = shiftedHarmonicCentroid(
        std::span<const double>{preserveRegular}.subspan(
            analysisStart, 16384),
        sampleRate,
        fundamental * ratio);
    require(std::abs(preserveCentroid - sourceCentroid)
                < std::abs(scaleCentroid - sourceCentroid),
            "preserve policy should keep the broad harmonic envelope centroid closer to its absolute source frequency");
    require(scaleStrongest >= 1900.0,
            "scale policy should move the primary broad envelope upward with pitch");
    double maximumPreservedSampleDelta = 0.0;
    for (std::size_t frame = analysisStart + 1;
         frame < analysisStart + 16384;
         ++frame)
    {
        require(std::isfinite(preserveRegular[frame]),
                "preserved formant output should remain finite");
        maximumPreservedSampleDelta = std::max(
            maximumPreservedSampleDelta,
            std::abs(preserveRegular[frame]
                     - preserveRegular[frame - 1]));
    }
    require(maximumPreservedSampleDelta < 0.5,
            "fixed formant preservation should retain bounded sample continuity");

    constexpr std::size_t totalFrames = sourceFrames
        + 2 * talas::dsp::phaseVocoderPitchFrameSize;
    constexpr std::array<std::size_t, 5> sectionStarts{
        0, 8192, 16384, 24576, totalFrames};
    constexpr std::array<double, 4> intervals{3.0, 7.0, -2.0, 5.0};
    const auto rampRegular = renderIntervalSections(
        input,
        sectionStarts,
        intervals,
        {256},
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency);
    const auto rampIrregular = renderIntervalSections(
        input,
        sectionStarts,
        intervals,
        {3, 509, 1, 257, 64, 17},
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency);
    require(rampRegular.output == rampIrregular.output,
            "dynamic preserved formant ramps should be partition invariant");
}

void testPeakIdentityBirthDeathAndResetAreClosed()
{
    constexpr std::size_t sectionFrames = 8192;
    constexpr std::size_t totalFrames = 3 * sectionFrames;
    std::vector<double> input(totalFrames, 0.0);
    for (std::size_t frame = 0; frame < sectionFrames; ++frame)
    {
        const auto firstPhase = 2.0 * std::numbers::pi * 187.5
            * static_cast<double>(frame) / 48000.0;
        const auto secondPhase = 2.0 * std::numbers::pi * 281.25
            * static_cast<double>(frame) / 48000.0;
        input[frame] = 0.15 * std::sin(firstPhase)
            + 0.10 * std::sin(2.0 * firstPhase)
            + 0.08 * std::sin(3.0 * firstPhase);
        input[2 * sectionFrames + frame] = 0.15 * std::sin(secondPhase)
            + 0.10 * std::sin(2.0 * secondPhase)
            + 0.08 * std::sin(3.0 * secondPhase);
    }

    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        48000.0, 256, 7.0};
    std::array<double, 256> output{};
    std::size_t firstDomainMatches = 0;
    for (std::size_t offset = 0; offset < input.size(); offset += 256)
    {
        require(
            processor.process(
                std::span<const double>{input}.subspan(offset, 256),
                {95, 7, offset},
                0,
                output)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "peak birth/death fixture should process");
        if (offset + 256 == sectionFrames)
        {
            firstDomainMatches = processor.peakIdentityMatchCount();
            require(processor.activePeakIdentityCount() >= 2,
                    "steady multi-peak material should publish identities");
        }
        if (offset + 256 == 2 * sectionFrames)
        {
            require(processor.activePeakIdentityCount() == 0,
                    "silent gap should expire previous peak identities");
        }
    }
    require(processor.peakIdentityMatchCount() > firstDomainMatches,
            "new harmonic material should establish a fresh identity domain");
    require(processor.activePeakIdentityCount() >= 2,
            "the second steady domain should publish fresh peak identities");

    std::array<double, 256> rejected{};
    rejected.fill(42.0);
    require(
        processor.process(
            std::span<const double>{input}.first(256),
            {95, 7, input.size() + 256},
            0,
            rejected)
            == talas::dsp::PhaseVocoderPitchStatus::DiscontinuousBlock,
        "timestamp discontinuity should fail closed");
    require(std::ranges::all_of(rejected, [](double sample) {
                return sample == 42.0;
            }),
            "timestamp discontinuity should not publish partial output");
    require(processor.activePeakIdentityCount() == 0,
            "timestamp discontinuity should revoke active peak identities");
    processor.reset();
    require(processor.peakIdentityMatchCount() == 0,
            "reset should clear peak-match observations");
    require(processor.activePeakIdentityCount() == 0,
            "reset should clear active peak identities");
}

void testTransientPolicyIsBoundedAndPartitionInvariant()
{
    constexpr std::size_t inputFrames = 16384;
    constexpr std::size_t firstOnset = 4096;
    constexpr std::size_t secondOnset = 10240;
    std::vector<double> impulses(inputFrames, 0.0);
    impulses[firstOnset] = 1.0;
    impulses[secondOnset] = -0.75;

    const auto regular = renderTransientFixture(impulses, {256});
    const auto irregular = renderTransientFixture(
        impulses,
        {1, 511, 17, 64, 3, 257});
    require(regular.output == irregular.output,
            "transient policy should be callback-partition invariant");
    require(regular.phaseAnchors == irregular.phaseAnchors,
            "transient decisions should be callback-partition invariant");
    require(regular.phaseAnchors == 3,
            "first frame and two separated attacks should anchor phase exactly once each");
    require(regular.matchedPeakIdentities == 0,
            "isolated transient frames should not publish reusable peak identities");
    require(regular.activePeakIdentities == 0,
            "isolated transient tail should retain no peak identity evidence");

    const auto firstNonZero = std::ranges::find_if(
        regular.output,
        [](double sample) { return std::abs(sample) > 1.0e-12; });
    const auto firstOutputFrame = static_cast<std::size_t>(
        std::distance(regular.output.begin(), firstNonZero));
    require(firstOutputFrame >= firstOnset,
            "causal transient processing should not publish before the source onset");
    require(firstOutputFrame <= firstOnset
                + talas::dsp::phaseVocoderPitchFrameSize,
            "transient attack should remain within one analysis frame of its source onset");
    const auto attackEnd = std::min(
        firstOutputFrame + talas::dsp::phaseVocoderPitchFrameSize,
        regular.output.size());
    require(std::ranges::any_of(
                std::span<const double>{regular.output}.subspan(
                    firstOutputFrame,
                    attackEnd - firstOutputFrame),
                [](double sample) { return std::abs(sample) > 1.0e-8; }),
            "accepted transient should remain audible inside one analysis frame");

    const auto steadyInput = makeTone(48000.0, 375.0, inputFrames);
    const auto steady = renderTransientFixture(
        std::span<const double>{steadyInput}.first(inputFrames),
        {256});
    require(steady.phaseAnchors == 1,
            "steady tone should retain continuous propagation after its initial anchor");
    require(steady.identityLockedFrames == 0,
            "single-partial material should retain independent phase propagation");

    talas::dsp::PreparedPhaseVocoderPitchShifter resetCandidate{
        48000.0, 256, 7.0};
    std::vector<double> firstPass(inputFrames, 0.0);
    std::vector<double> secondPass(inputFrames, 0.0);
    for (std::size_t offset = 0; offset < inputFrames; offset += 256)
    {
        require(resetCandidate.process(
                    std::span<const double>{impulses}.subspan(offset, 256),
                    {92, 1, offset},
                    0,
                    std::span<double>{firstPass}.subspan(offset, 256))
                    == talas::dsp::PhaseVocoderPitchStatus::Ok,
                "first transient reset fixture pass should process");
    }
    require(resetCandidate.transientPhaseAnchorCount() == 3,
            "first transient pass should observe the fixed anchors");
    resetCandidate.reset();
    require(resetCandidate.transientPhaseAnchorCount() == 0,
            "reset should clear transient decision history");
    require(resetCandidate.identityLockedFrameCount() == 0,
            "reset should clear identity-lock observation history");
    require(resetCandidate.peakIdentityMatchCount() == 0,
            "reset should clear cross-frame peak-match history");
    require(resetCandidate.activePeakIdentityCount() == 0,
            "reset should clear live cross-frame peak identities");
    for (std::size_t offset = 0; offset < inputFrames; offset += 256)
    {
        require(resetCandidate.process(
                    std::span<const double>{impulses}.subspan(offset, 256),
                    {93, 8, offset + 7000},
                    0,
                    std::span<double>{secondPass}.subspan(offset, 256))
                    == talas::dsp::PhaseVocoderPitchStatus::Ok,
                "second transient reset fixture pass should process");
    }
    require(firstPass == secondPass,
            "reset should reproduce transient output without prior onset evidence");
    require(resetCandidate.transientPhaseAnchorCount() == 3,
            "reset fixture should reproduce transient decisions");
}

void testKnownToneRatiosAndPartitionInvariance()
{
    for (const auto sampleRate : {44100.0, 48000.0})
    {
        for (const auto sourceFrequency : {
                 sampleRate / 128.0,
                 sampleRate * 9.375 / 1024.0,
             })
        {
            const auto input = makeTone(sampleRate, sourceFrequency, 32768);
            for (const auto interval : {-12.0, -7.0, 7.0, 12.0})
            {
                const auto regular = render(
                    sampleRate, interval, input, {512});
                const auto irregular = render(
                    sampleRate, interval, input, {1, 257, 17, 509, 64, 3});
                require(regular.size() == irregular.size(),
                        "partition fixtures should retain duration");
                for (std::size_t frame = 0; frame < regular.size(); ++frame)
                {
                    require(
                        std::abs(regular[frame] - irregular[frame]) < 1.0e-9,
                        "phase-vocoder output should be partition invariant");
                }

                const auto expected = sourceFrequency
                    * talas::dsp::phaseVocoderPitchRatio(interval);
                const auto analysisStart =
                    talas::dsp::phaseVocoderPitchLatencyFrames + 8192;
                const auto measured = estimateFrequency(
                    std::span<const double>{regular}.subspan(
                        analysisStart, 16384),
                    sampleRate,
                    expected);
                require(std::abs(measured - expected) / expected < 0.015,
                        "shifted tone should match the requested pitch ratio");
                requireContinuousHopBoundaries(
                    regular,
                    analysisStart,
                    16384);
            }
        }
    }
}

void testBoundedRealtimeCallbackCost()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockFrames = 256;
    constexpr std::size_t measuredCallbacks = 2048;
    constexpr auto callbackBudgetNanoseconds = 5'333'333LL;
    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        sampleRate,
        blockFrames,
        7.0,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency};
    const auto inputStorage = makeTone(sampleRate, 440.0, blockFrames);
    const auto input = std::span<const double>{inputStorage}.first(blockFrames);
    std::array<double, blockFrames> output{};
    std::array<long long, measuredCallbacks> durations{};

    std::size_t sourceFrame = 0;
    for (std::size_t callback = 0;
         callback < measuredCallbacks + 8;
         ++callback)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto status = processor.process(
            input,
            {42, 1, sourceFrame},
            0,
            output);
        const auto finished = std::chrono::steady_clock::now();
        require(status == talas::dsp::PhaseVocoderPitchStatus::Ok,
                "bounded callback benchmark should process");
        if (callback >= 8)
        {
            durations[callback - 8] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - started).count();
        }
        sourceFrame += blockFrames;
    }

    std::ranges::sort(durations);
    const auto p99 = durations[(durations.size() * 99) / 100];
    std::cout << "D4 pitch callback p99: " << p99
              << " ns; local 48 kHz / 256-frame budget: "
              << callbackBudgetNanoseconds << " ns\n";
    if (!sanitizerInstrumented && strictTiming())
    {
        require(p99 < callbackBudgetNanoseconds,
                "phase-vocoder callback p99 should fit the local core budget");
    }

    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter dynamicProcessor{
        sampleRate, blockFrames, 0.0};
    sourceFrame = 0;
    for (std::size_t callback = 0;
         callback < measuredCallbacks + 8;
         ++callback)
    {
        const auto interval = ((callback / 32) % 2) == 0 ? 7.0 : -7.0;
        const auto started = std::chrono::steady_clock::now();
        const auto status = dynamicProcessor.process(
            input,
            interval,
            {43, 1, sourceFrame},
            0,
            output);
        const auto finished = std::chrono::steady_clock::now();
        require(status == talas::dsp::PhaseVocoderPitchStatus::Ok,
                "dynamic callback benchmark should process");
        if (callback >= 8)
        {
            durations[callback - 8] =
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    finished - started).count();
        }
        sourceFrame += blockFrames;
    }
    std::ranges::sort(durations);
    const auto dynamicP99 = durations[(durations.size() * 99) / 100];
    std::cout << "D4 dynamic pitch callback p99: " << dynamicP99
              << " ns; local 48 kHz / 256-frame budget: "
              << callbackBudgetNanoseconds << " ns\n";
    if (!sanitizerInstrumented && strictTiming())
    {
        require(dynamicP99 < callbackBudgetNanoseconds,
                "dynamic phase-vocoder callback p99 should fit the local core budget");
    }
}

void testPreparedProcessingAndResetDoNotAllocate()
{
    constexpr std::size_t blockFrames = 256;
    constexpr std::size_t callbackCount = 16;
    const auto input = makeHarmonicStack(
        48000.0, 187.5, blockFrames * callbackCount);
    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        48000.0,
        blockFrames,
        7.0,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency};
    std::array<double, blockFrames> output{};
    std::array<talas::dsp::PhaseVocoderPitchStatus, callbackCount> statuses{};

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    for (std::size_t callback = 0;
         callback < callbackCount;
         ++callback)
    {
        const auto offset = callback * blockFrames;
        statuses[callback] = processor.process(
            std::span<const double>{input}.subspan(offset, blockFrames),
            {44, 2, offset},
            0,
            output);
    }
    processor.reset();
    countAllocations.store(false, std::memory_order_release);

    require(std::ranges::all_of(statuses, [](const auto status) {
                return status == talas::dsp::PhaseVocoderPitchStatus::Ok;
            }),
            "prepared allocation fixture should process every callback");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "prepared phase-vocoder processing and reset must not allocate");
}

void testDynamicIntervalsAreFixedLatencyAndPartitionInvariant()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t sectionFrames = 16384;
    constexpr std::array<std::size_t, 5> sectionStarts{
        0,
        sectionFrames,
        2 * sectionFrames,
        3 * sectionFrames,
        4 * sectionFrames,
    };
    constexpr std::array<double, 4> intervals{0.0, 7.0, -7.0, 0.0};
    const auto sourceFrequency = sampleRate * 9.375 / 1024.0;
    const auto inputStorage = makeTone(
        sampleRate, sourceFrequency, sectionStarts.back());
    const auto input = std::span<const double>{inputStorage}.first(
        sectionStarts.back());
    const auto contiguous = renderDynamic(
        sampleRate,
        input,
        sectionStarts,
        intervals,
        {512});
    const auto irregular = renderDynamic(
        sampleRate,
        input,
        sectionStarts,
        intervals,
        {3, 509, 1, 257, 64, 17});
    require(contiguous.size() == irregular.size(),
            "dynamic pitch partitions should retain duration");
    for (std::size_t frame = 0; frame < contiguous.size(); ++frame)
    {
        require(std::abs(contiguous[frame] - irregular[frame]) < 1.0e-9,
                "dynamic pitch output should be partition invariant");
    }

    for (std::size_t frame = talas::dsp::phaseVocoderPitchLatencyFrames;
         frame < sectionFrames;
         ++frame)
    {
        require(
            contiguous[frame]
                == input[frame - talas::dsp::phaseVocoderPitchLatencyFrames],
            "settled dynamic neutral output should be an exact delayed copy");
    }
    const auto finalNeutralStart = 3 * sectionFrames
        + talas::dsp::dynamicPhaseVocoderPitchTransitionFrames;
    for (std::size_t frame = finalNeutralStart;
         frame < sectionStarts.back();
         ++frame)
    {
        require(
            contiguous[frame]
                == input[frame - talas::dsp::phaseVocoderPitchLatencyFrames],
            "dynamic return to neutral should settle to the exact delayed copy");
    }
    for (const auto boundary : {
             sectionFrames,
             2 * sectionFrames,
             3 * sectionFrames,
         })
    {
        double maximumDelta = 0.0;
        for (std::size_t frame = boundary - 2048;
             frame < boundary + 2048;
             ++frame)
        {
            maximumDelta = std::max(
                maximumDelta,
                std::abs(contiguous[frame] - contiguous[frame - 1]));
        }
        require(maximumDelta < 0.25,
                "dynamic pitch transition should have bounded sample discontinuity");
    }

    const auto positiveMeasured = estimateFrequency(
        std::span<const double>{contiguous}.subspan(
            sectionFrames + 4096,
            8192),
        sampleRate,
        sourceFrequency * talas::dsp::phaseVocoderPitchRatio(7.0));
    require(
        std::abs(
            positiveMeasured
            - sourceFrequency * talas::dsp::phaseVocoderPitchRatio(7.0))
            / (sourceFrequency * talas::dsp::phaseVocoderPitchRatio(7.0))
            < 0.02,
        "settled dynamic positive interval should match its pitch ratio");
    const auto negativeMeasured = estimateFrequency(
        std::span<const double>{contiguous}.subspan(
            2 * sectionFrames + 4096,
            8192),
        sampleRate,
        sourceFrequency * talas::dsp::phaseVocoderPitchRatio(-7.0));
    require(
        std::abs(
            negativeMeasured
            - sourceFrequency * talas::dsp::phaseVocoderPitchRatio(-7.0))
            / (sourceFrequency * talas::dsp::phaseVocoderPitchRatio(-7.0))
            < 0.02,
        "settled dynamic negative interval should match its pitch ratio");
}

void testDynamicConstantIntervalsAtAcceptedSampleRates()
{
    constexpr std::array<std::size_t, 5> sectionStarts{
        0, 8192, 16384, 24576, 32768};
    for (const auto sampleRate : {44100.0, 48000.0})
    {
        const auto sourceFrequency = sampleRate * 9.375 / 1024.0;
        const auto inputStorage = makeTone(
            sampleRate, sourceFrequency, sectionStarts.back());
        const auto input = std::span<const double>{inputStorage}.first(
            sectionStarts.back());
        for (const auto interval : {-12.0, -7.0, 0.0, 7.0, 12.0})
        {
            const std::array<double, 4> intervals{
                interval, interval, interval, interval};
            const auto output = renderDynamic(
                sampleRate,
                input,
                sectionStarts,
                intervals,
                {256});
            if (interval == 0.0)
            {
                for (std::size_t frame =
                         talas::dsp::phaseVocoderPitchLatencyFrames;
                     frame < output.size();
                     ++frame)
                {
                    require(
                        output[frame]
                            == input[frame
                                - talas::dsp::phaseVocoderPitchLatencyFrames],
                        "dynamic constant neutral interval should remain an exact delayed copy");
                }
                continue;
            }
            const auto expected = sourceFrequency
                * talas::dsp::phaseVocoderPitchRatio(interval);
            const auto measured = estimateFrequency(
                std::span<const double>{output}.subspan(8192, 16384),
                sampleRate,
                expected);
            require(std::abs(measured - expected) / expected < 0.015,
                    "dynamic constant interval should match its requested ratio");
        }
    }
}

void testDynamicRejectedControlDoesNotAdvanceState()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockFrames = 256;
    const auto inputStorage = makeTone(sampleRate, 440.0, 2 * blockFrames);
    const auto input = std::span<const double>{inputStorage}.first(
        2 * blockFrames);
    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter clean{
        sampleRate, blockFrames, 0.0};
    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter candidate{
        sampleRate, blockFrames, 0.0};
    std::array<double, blockFrames> cleanFirst{};
    std::array<double, blockFrames> cleanSecond{};
    std::array<double, blockFrames> candidateFirst{};
    std::array<double, blockFrames> candidateSecond{};
    std::array<double, blockFrames> rejected{};
    rejected.fill(42.0);

    require(clean.process(input.first(blockFrames), 7.0, {1, 1, 0}, 0,
                          cleanFirst)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "clean dynamic first block should process");
    require(candidate.process(input.first(blockFrames), 7.0, {1, 1, 0}, 0,
                              candidateFirst)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "candidate dynamic first block should process");
    require(candidate.process(
                input.subspan(blockFrames, blockFrames),
                std::numeric_limits<double>::quiet_NaN(),
                {1, 1, blockFrames},
                0,
                rejected)
                == talas::dsp::PhaseVocoderPitchStatus::InvalidInterval,
            "non-finite dynamic interval should fail closed");
    require(std::ranges::all_of(rejected, [](double sample) {
                return sample == 42.0;
            }),
            "rejected dynamic interval should not publish partial output");
    require(clean.process(input.subspan(blockFrames, blockFrames), 7.0,
                          {1, 1, blockFrames}, 0, cleanSecond)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "clean dynamic second block should process");
    require(candidate.process(input.subspan(blockFrames, blockFrames), 7.0,
                              {1, 1, blockFrames}, 0, candidateSecond)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "candidate dynamic retry should process");
    require(cleanFirst == candidateFirst && cleanSecond == candidateSecond,
            "rejected dynamic interval should not advance prepared state");
}

void testFrequencyScaleRatioAuthorityIsClosed()
{
    for (const auto ratio : {0.5, 0.75, 1.0, 1.5, 2.0})
    {
        require(talas::dsp::isSupportedPhaseVocoderFrequencyScale(ratio),
                "FrequencyScale ratio should be admitted");
        const auto interval =
            talas::dsp::phaseVocoderFrequencyScaleInterval(ratio);
        require(talas::dsp::isSupportedPhaseVocoderPitchInterval(interval),
                "FrequencyScale ratio should map into the pitch interval profile");
        require(std::abs(talas::dsp::phaseVocoderPitchRatio(interval) - ratio)
                    < 1.0e-12,
                "FrequencyScale ratio/interval conversion should round-trip");
    }
    for (const auto ratio : {
             0.0,
             0.49,
             2.01,
             std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN(),
         })
    {
        require(!talas::dsp::isSupportedPhaseVocoderFrequencyScale(ratio),
                "out-of-profile FrequencyScale ratio should be rejected");
        require(std::isnan(
                    talas::dsp::phaseVocoderFrequencyScaleInterval(ratio)),
                "rejected FrequencyScale ratio should not produce an interval");
    }
}

void testKnownPitchRatioAuthorityIsClosed()
{
    require(talas::dsp::phaseVocoderKnownPitchRatio(220.0, 440.0, 440.0)
                == 0.5,
            "known-pitch lower-octave ratio should be admitted");
    require(talas::dsp::phaseVocoderKnownPitchRatio(660.0, 440.0, 440.0)
                == 1.5,
            "known-pitch upper target ratio should be admitted");
    for (const auto ratio : {
             talas::dsp::phaseVocoderKnownPitchRatio(219.0, 440.0, 440.0),
             talas::dsp::phaseVocoderKnownPitchRatio(881.0, 440.0, 440.0),
             talas::dsp::phaseVocoderKnownPitchRatio(440.0, 220.0, 440.0),
             talas::dsp::phaseVocoderKnownPitchRatio(440.0, 0.0, 0.0),
         })
    {
        require(std::isnan(ratio),
                "invalid known-pitch authority should fail closed");
    }
}

void testMonotonizerBlockControlAuthorityIsClosed()
{
    using Status = talas::dsp::MonotonizerControlStatus;
    constexpr std::array<double, 4> target{150.0, 150.0, 150.0, 150.0};
    constexpr std::array<double, 4> source{100.0, 100.0, 100.0, 100.0};
    auto frequency = talas::dsp::MonotonizerControlInput{
        std::numeric_limits<double>::infinity(), target, true};
    auto minimum = talas::dsp::MonotonizerControlInput{
        std::numeric_limits<double>::infinity(), source, true};
    auto maximum = talas::dsp::MonotonizerControlInput{
        std::numeric_limits<double>::infinity(), source, true};
    talas::dsp::MonotonizerBlockDecision decision;
    require(talas::dsp::checkedMonotonizerBlockDecision(
                48000.0,
                target.size(),
                true,
                frequency,
                minimum,
                maximum,
                decision)
                == Status::Ok
            && decision.activeProfile
            && decision.frequency == 150.0
            && decision.sourcePitch == 100.0
            && decision.ratio == 1.5
            && decision.intervalSemitones
                == talas::dsp::phaseVocoderFrequencyScaleInterval(1.5),
        "C105 connected Monotonizer authority should ignore hidden fallbacks and publish one exact block decision");

    auto changedTarget = target;
    changedTarget[3] = 125.0;
    frequency.connectedValues = changedTarget;
    require(talas::dsp::checkedMonotonizerBlockDecision(
                48000.0,
                changedTarget.size(),
                true,
                frequency,
                minimum,
                maximum,
                decision)
                == Status::FrequencyNotBlockConstant,
        "C105 Monotonizer authority should reject the exact connected control that changes within a block");

    frequency = {150.0, {}, false};
    minimum = {100.0, {}, false};
    maximum = {101.0, {}, false};
    require(talas::dsp::checkedMonotonizerBlockDecision(
                48000.0,
                4,
                true,
                frequency,
                minimum,
                maximum,
                decision)
                == Status::NonSingletonInputPitch,
        "C105 Monotonizer authority should reject non-singleton source evidence before deriving a ratio");

    frequency = {24000.0, {}, false};
    minimum = {16000.0, {}, false};
    maximum = {16000.0, {}, false};
    require(talas::dsp::checkedMonotonizerBlockDecision(
                48000.0,
                4,
                true,
                frequency,
                minimum,
                maximum,
                decision)
                == Status::NyquistExceeded,
        "C105 active Monotonizer authority should reject a target at Nyquist");

    frequency = {100.0, {}, false};
    minimum = {100.0, {}, false};
    maximum = {100.0, {}, false};
    require(talas::dsp::checkedMonotonizerBlockDecision(
                48000.0,
                4,
                false,
                frequency,
                minimum,
                maximum,
                decision)
                == Status::Ok
            && !decision.activeProfile
            && decision.ratio == 1.0
            && decision.intervalSemitones == 0.0,
        "C105 static neutral Monotonizer authority should preserve exact zero-latency identity");
}

void testActivePitchStartupUsesBoundedSynthesisWindow()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockFrames = 256;
    constexpr std::size_t totalFrames = 1536;
    const auto input = makeTone(sampleRate, 468.75, totalFrames);
    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter processor{
        sampleRate,
        blockFrames,
        talas::dsp::phaseVocoderFrequencyScaleInterval(1.5),
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency};
    std::array<double, totalFrames> output{};
    for (std::size_t offset = 0; offset < totalFrames;
         offset += blockFrames)
    {
        require(processor.process(
                    std::span<const double>{input}.subspan(
                        offset, blockFrames),
                    talas::dsp::phaseVocoderFrequencyScaleInterval(1.5),
                    {0, 0, offset},
                    0,
                    std::span<double>{output}.subspan(
                        offset, blockFrames))
                    == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "C105 active startup fixture should process");
    }
    require(std::ranges::all_of(output, [](double sample) {
                return std::isfinite(sample)
                    && std::abs(sample) <= 1.0;
            }),
        "C105 phase-vocoder synthesis must taper modified frames before overlap normalization so startup stays bounded");
}

void testEmptyDynamicPitchBlockConsumesNoTargetState()
{
    constexpr double sampleRate = 48000.0;
    constexpr std::size_t blockFrames = 256;
    constexpr std::size_t totalFrames = 1536;
    const auto activeInterval =
        talas::dsp::phaseVocoderFrequencyScaleInterval(1.5);
    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter withEmpty{
        sampleRate,
        blockFrames,
        activeInterval,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency};
    talas::dsp::PreparedDynamicPhaseVocoderPitchShifter reference{
        sampleRate,
        blockFrames,
        activeInterval,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::
            PreserveAbsoluteFrequency};
    require(withEmpty.process(
                {},
                0.0,
                {0, 0, 0},
                0,
                {})
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
        "C105 empty dynamic pitch block should be an accepted no-op");

    const auto input = makeTone(sampleRate, 468.75, totalFrames);
    std::array<double, totalFrames> actual{};
    std::array<double, totalFrames> expected{};
    for (std::size_t offset = 0; offset < totalFrames;
         offset += blockFrames)
    {
        require(withEmpty.process(
                    std::span<const double>{input}.subspan(
                        offset, blockFrames),
                    activeInterval,
                    {0, 0, offset},
                    0,
                    std::span<double>{actual}.subspan(
                        offset, blockFrames))
                    == talas::dsp::PhaseVocoderPitchStatus::Ok
                && reference.process(
                    std::span<const double>{input}.subspan(
                        offset, blockFrames),
                    activeInterval,
                    {0, 0, offset},
                    0,
                    std::span<double>{expected}.subspan(
                        offset, blockFrames))
                    == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "C105 empty-block state fixture should render");
    }
    require(actual == expected,
        "C105 empty dynamic pitch block must not retarget ratio, mix, phase, overlap, or transport state");

    talas::dsp::MonotonizerBlockDecision decision;
    const auto hidden = std::numeric_limits<double>::infinity();
    require(talas::dsp::checkedMonotonizerBlockDecision(
                sampleRate,
                0,
                true,
                {hidden, {}, true},
                {hidden, {}, true},
                {hidden, {}, true},
                decision)
                == talas::dsp::MonotonizerControlStatus::Ok
            && decision.activeProfile
            && decision.ratio == 1.0
            && decision.intervalSemitones == 0.0,
        "C105 empty connected control decision should ignore hidden fallbacks and remain a canonical no-op");
}

void testTimeFrequencyCorrectionAuthorityIsClosed()
{
    using namespace talas::dsp;
    require(phaseVocoderTimeFrequencyCorrectionRatio(1, 0.25) == 4
            && phaseVocoderTimeFrequencyCorrectionInterval(4) == 24
            && std::isnan(phaseVocoderFrequencyScaleInterval(4))
            && std::isnan(phaseVocoderKnownPitchRatio(400, 100, 100))
            && !isSupportedPhaseVocoderPitchInterval(24)
            && isSupportedPhaseVocoderPitchInterval(24, PhaseVocoderPitchProfile::TimeFrequencyCorrection),
        "quarter-speed correction must be confined to the explicit TimeFrequencyScale profile");
    require(talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.0, 0.5)
                == 2.0,
            "half-rate playback should admit an octave-up correction");
    require(talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.0, 2.0)
                == 0.5,
            "double-rate playback should admit an octave-down correction");
    require(talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.5, 1.0)
                == 1.5,
            "independent target pitch should admit a bounded correction");
    for (const auto ratio : {
             talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.0, 0.0),
             talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.0, -1.0),
             talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.0, 0.249),
             talas::dsp::phaseVocoderTimeFrequencyCorrectionRatio(1.0, 2.01),
         })
    {
        require(std::isnan(ratio),
                "invalid time/frequency correction should fail closed");
    }
    require(std::isnan(phaseVocoderTimeFrequencyCorrectionInterval(4.001))
            && std::isnan(phaseVocoderTimeFrequencyCorrectionInterval(0.499))
            && !isSupportedPhaseVocoderPitchInterval(24.001, PhaseVocoderPitchProfile::TimeFrequencyCorrection)
            && !isSupportedPhaseVocoderPitchInterval(-12.001, PhaseVocoderPitchProfile::TimeFrequencyCorrection)
            && !isSupportedPhaseVocoderPitchInterval(0, static_cast<PhaseVocoderPitchProfile>(99)),
        "explicit time-frequency correction authority must remain closed");

    const auto input = makeTone(48000, 93.75, 16384);
    const auto regular = render(48000, 24, input, {256}, PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    const auto irregular = render(48000, 24, input, {1, 127, 511, 31}, PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    require(regular == irregular, "two-octave correction must be partition invariant");
    require(std::ranges::all_of(std::span<const double>{regular}.first(phaseVocoderPitchLatencyFrames),
        [](double sample) { return sample == 0; }), "two-octave correction must retain1023-frame latency");
    const auto steady = std::span<const double>{regular}.subspan(4096, 8192);
    const auto measured = estimateFrequency(steady, 48000, 375);
    require(std::abs(measured - 375) <= 1, "two-octave correction must restore independent known pitch");
    require(std::abs(projectedAmplitude(steady, 48000, 375) - 0.25) < 1e-8,
        "translated bin-aligned peak must preserve the known native tone amplitude");
    for (const auto probe : {0.0, 93.75, 187.5, 281.25, 468.75, 562.5, 750.0, 937.5})
        require(projectedAmplitude(steady, 48000, probe) < 1e-8,
            "extended correction must not introduce the measured hop-rate sidebands");

    for (const auto sourceFrequency : {55.0, 110.0, 173.0})
        for (const auto ratio : {2.5, 3.5, 4.0})
        {
            const auto tone = makeTone(48000, sourceFrequency, 16384);
            const auto shifted = render(48000, phaseVocoderTimeFrequencyCorrectionInterval(ratio),
                tone, {127, 511, 31}, PhaseVocoderPitchProfile::TimeFrequencyCorrection);
            const auto window = std::span<const double>{shifted}.subspan(4096, 8192);
            const auto wanted = sourceFrequency * ratio;
            const auto amplitude = projectedAmplitude(window, 48000, wanted);
            double peakFrequency = 0;
            double peakAmplitude = 0;
            for (auto probe = wanted - 12; probe <= wanted + 12; probe += 0.5)
            {
                const auto projected = projectedAmplitude(window, 48000, probe);
                if (projected > peakAmplitude) { peakAmplitude = projected; peakFrequency = probe; }
            }
            require(amplitude > 0.02 && std::abs(peakFrequency - wanted) <= 1
                    && amplitude > 10 * projectedAmplitude(window, 48000, sourceFrequency),
                "off-bin low tones and intermediate extended ratios must retain their requested frequency");
        }

    auto pair = makeTone(48000, 93.75, 16384);
    const auto second = makeTone(48000, 234.375, 16384);
    for (std::size_t i = 0; i < pair.size(); ++i) pair[i] += second[i];
    const auto shiftedPair = render(48000, 24, pair, {127, 511, 31}, PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    const auto pairWindow = std::span<const double>{shiftedPair}.subspan(4096, 8192);
    require(projectedAmplitude(pairWindow, 48000, 375) > 0.2
            && projectedAmplitude(pairWindow, 48000, 937.5) > 0.2
            && projectedAmplitude(pairWindow, 48000, 187.5) < 1e-8
            && projectedAmplitude(pairWindow, 48000, 562.5) < 1e-8,
        "separate translated peak regions must retain both tones without duplicated lobes");

    const auto transitionTone = makeTone(48000, 93.75, 20000);
    const std::array<std::size_t, 5> starts{0, 4096, 8192, 12288, transitionTone.size()};
    const std::array<double, 4> intervals{12, 24, phaseVocoderTimeFrequencyCorrectionInterval(3.5), 12};
    const auto transitions = renderDynamic(48000, transitionTone, starts, intervals, {256},
        PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    const auto irregularTransitions = renderDynamic(48000, transitionTone, starts, intervals, {31, 127, 511},
        PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    require(transitions == irregularTransitions,
        "transitions across two-octave mapping and rounded peak shifts must retain partition invariance");
    requireContinuousHopBoundaries(transitions, 2048, 16000);
    require(std::ranges::all_of(transitions, [](double value) { return std::abs(value) < 1; }),
        "extended correction transitions must retain a bounded signal");

    auto offsetTone = makeTone(48000, 234.375, 16384);
    for (std::size_t i = 0; i < 16384; ++i) offsetTone[i] += 0.05;
    const auto shiftedOffset = render(48000, 24, offsetTone, {127, 511, 31},
        PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    const auto offsetWindow = std::span<const double>{shiftedOffset}.subspan(4096, 8192);
    double mean = 0;
    for (const auto value : offsetWindow) mean += value / offsetWindow.size();
    require(std::abs(mean - 0.05) < 1e-8
            && projectedAmplitude(offsetWindow, 48000, 937.5) > 0.2,
        "extended peak translation must preserve a DC offset without retuning it");
    for (const auto value : {-0.05, 0.05})
    {
        std::vector<double> constant(16384, value);
        const auto shifted = render(48000, 24, constant, {127, 511, 31},
            PhaseVocoderPitchProfile::TimeFrequencyCorrection);
        require(std::ranges::all_of(std::span<const double>{shifted}.subspan(4096),
            [value](double sample) { return std::abs(sample - value) < 1e-10; }),
            "extended correction must preserve either polarity of a steady constant");
    }
    auto quietTone = makeTone(48000, 234.375, 16384);
    for (std::size_t i = 0; i < 16384; ++i) quietTone[i] = 0.5 + quietTone[i] * 0.004;
    const auto shiftedQuiet = render(48000, 24, quietTone, {127, 511, 31},
        PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    const auto quietWindow = std::span<const double>{shiftedQuiet}.subspan(4096, 8192);
    require(std::abs(projectedAmplitude(quietWindow, 48000, 937.5) - 0.001) < 1e-8
            && projectedAmplitude(quietWindow, 48000, 234.375) < 1e-8,
        "a quiet partial below the old relative peak floor must still shift above dominant DC");

    PreparedPhaseVocoderPitchShifter standard{48000, 128, 12};
    PreparedPhaseVocoderPitchShifter extended{48000, 128, 12,
        PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum, PhaseVocoderPitchProfile::TimeFrequencyCorrection};
    require(!standard.copyProcessingStateFrom(extended) && !extended.copyProcessingStateFrom(standard),
        "state copies must reject unequal correction profiles even with equal intervals");
    PreparedDynamicPhaseVocoderPitchShifter dynamicStandard{48000, 128, 12};
    PreparedDynamicPhaseVocoderPitchShifter dynamicExtended{48000, 128, 12,
        PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum, PhaseVocoderPitchProfile::TimeFrequencyCorrection};
    require(!dynamicStandard.copyProcessingStateFrom(dynamicExtended)
            && !dynamicExtended.copyProcessingStateFrom(dynamicStandard),
        "dynamic state copies must reject unequal correction profiles");
}

void testLatencyResetAndFailuresAreClosed()
{
    constexpr double sampleRate = 48000.0;
    auto input = makeTone(sampleRate, 375.0, 4096);
    auto output = render(sampleRate, 12.0, input, {127, 511, 31});
    for (std::size_t frame = 0;
         frame < talas::dsp::phaseVocoderPitchLatencyFrames;
         ++frame)
    {
        require(output[frame] == 0.0,
                "non-neutral startup latency should be exactly N-1 zeros");
    }

    talas::dsp::PreparedPhaseVocoderPitchShifter processor{
        sampleRate, 512, 12.0};
    std::vector<double> first(512, 0.0);
    std::vector<double> repeated(512, 0.0);
    require(processor.process(
                std::span<const double>{input}.first(512),
                {1, 2, 0},
                0,
                first)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "first prepared block should process");
    processor.reset();
    require(processor.process(
                std::span<const double>{input}.first(512),
                {9, 4, 700},
                0,
                repeated)
                == talas::dsp::PhaseVocoderPitchStatus::Ok,
            "reset should accept a new timestamp domain");
    require(first == repeated,
            "reset should remove all prior phase evidence");

    std::vector<double> rejected(512, 42.0);
    auto malformed = std::vector<double>(512, 0.0);
    malformed[17] = std::numeric_limits<double>::quiet_NaN();
    processor.reset();
    require(processor.process(
                malformed,
                {1, 1, 0},
                0,
                rejected)
                == talas::dsp::PhaseVocoderPitchStatus::NonFiniteInput,
            "non-finite input should fail closed");
    require(std::ranges::all_of(rejected, [](double sample) {
                return sample == 42.0;
            }),
            "rejected input should not publish partial output");

    bool threw = false;
    try
    {
        talas::dsp::PreparedPhaseVocoderPitchShifter invalid{
            sampleRate, 512, 13.0};
        (void) invalid;
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    require(threw, "unsupported interval should fail preparation");

    threw = false;
    try
    {
        talas::dsp::PreparedPhaseVocoderPitchShifter invalidPolicy{
            sampleRate,
            512,
            7.0,
            static_cast<talas::dsp::PhaseVocoderSpectralEnvelopePolicy>(99)};
        (void) invalidPolicy;
    }
    catch (const std::invalid_argument&)
    {
        threw = true;
    }
    require(threw, "unknown spectral-envelope policy should fail preparation");
}
template <class Processor>
void verifyOwnedProcessingClone(double initialInterval, talas::dsp::PhaseVocoderSpectralEnvelopePolicy policy,
    talas::dsp::PhaseVocoderPitchProfile profile = talas::dsp::PhaseVocoderPitchProfile::OneOctave)
{
    using namespace talas::dsp;
    constexpr std::size_t blockFrames = 257;
    auto input = makeTone(48000.0, 173.0, 10000);
    Processor candidate(48000.0, blockFrames, initialInterval, policy, profile);
    Processor reference(48000.0, blockFrames, initialInterval, policy, profile);
    std::array<double, blockFrames> actual{}, expected{};
    const auto process = [&](Processor& processor, std::size_t frame, std::size_t count,
                             double interval, std::span<double> output) {
        const auto source = std::span<const double>(input).subspan(frame, count);
        if constexpr (std::is_same_v<Processor, PreparedPhaseVocoderPitchShifter>)
            return processor.processWithInterval(source, interval, {71, 3, frame}, 0, output.first(count));
        else
            return processor.process(source, interval, {71, 3, frame}, 0, output.first(count));
    };
    for (std::size_t frame = 0; frame != 4096;)
    {
        const auto count = std::min(blockFrames, 4096 - frame);
        require(process(candidate, frame, count, initialInterval, actual) == PhaseVocoderPitchStatus::Ok
                && process(reference, frame, count, initialInterval, expected) == PhaseVocoderPitchStatus::Ok,
            "processing clone fixture must commit its initial spectral history");
        frame += count;
    }
    const auto changedInterval = initialInterval == 0.0 ? 7.0 : -7.0;
    require(process(candidate, 4096, 113, changedInterval, actual) == PhaseVocoderPitchStatus::Ok
            && process(reference, 4096, 113, changedInterval, expected) == PhaseVocoderPitchStatus::Ok,
        "processing clone must capture during a live interval transition");
    auto baseline = candidate.cloneProcessingState();
    for (std::size_t frame = 4209; frame != 5237; frame += blockFrames)
        require(process(candidate, frame, blockFrames, 0.0, actual) == PhaseVocoderPitchStatus::Ok,
            "processing clone fixture must overwrite live phase, overlap and transition state");
    allocationCount.store(0);
    countAllocations.store(true);
    const auto restored = candidate.copyProcessingStateFrom(*baseline);
    countAllocations.store(false);
    require(restored && allocationCount.load() == 0,
        "processing clone must restore without allocation using the original preparation identity");
    baseline.reset(); // Continuing must not borrow any state from the checkpoint.
    for (std::size_t frame = 4209; frame != 7000;)
    {
        const auto boundary = frame < 4800 ? 4800 : frame < 6000 ? 6000 : 7000;
        const auto count = std::min(blockFrames, boundary - frame);
        const auto interval = frame < 4800 ? changedInterval : frame < 6000 ? -3.0 : 0.0;
        allocationCount.store(0);
        countAllocations.store(true);
        const auto actualStatus = process(candidate, frame, count, interval, actual);
        const auto expectedStatus = process(reference, frame, count, interval, expected);
        countAllocations.store(false);
        require(actualStatus == PhaseVocoderPitchStatus::Ok && expectedStatus == PhaseVocoderPitchStatus::Ok
                && allocationCount.load() == 0,
            "restored clone continuation must retain allocation-free prepared processing");
        require(std::equal(actual.begin(), actual.begin() + count, expected.begin()),
            "owned clone must preserve exact phases, spectral envelopes, transition ramps and timestamps");
        frame += count;
    }
}

void testOwnedProcessingClonesPreserveChangedIntervals()
{
    verifyOwnedProcessingClone<talas::dsp::PreparedPhaseVocoderPitchShifter>(24,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
        talas::dsp::PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    verifyOwnedProcessingClone<talas::dsp::PreparedDynamicPhaseVocoderPitchShifter>(24,
        talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
        talas::dsp::PhaseVocoderPitchProfile::TimeFrequencyCorrection);
    for (const auto policy : {talas::dsp::PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum,
                             talas::dsp::PhaseVocoderSpectralEnvelopePolicy::PreserveAbsoluteFrequency})
        for (const auto initialInterval : {0.0, 7.0})
        {
            verifyOwnedProcessingClone<talas::dsp::PreparedPhaseVocoderPitchShifter>(initialInterval, policy);
            verifyOwnedProcessingClone<talas::dsp::PreparedDynamicPhaseVocoderPitchShifter>(initialInterval, policy);
        }
}

}

int main()
{
    testOwnedProcessingClonesPreserveChangedIntervals();
    testKnownToneRatiosAndPartitionInvariance();
    testIdentityPhaseLockingPreservesHarmonicCoherence();
    testCrossFramePeakIdentityTracksGlideAndRatioRamp();
    testFixedSpectralEnvelopePolicyPreservesBroadFormants();
    testPeakIdentityBirthDeathAndResetAreClosed();
    testTransientPolicyIsBoundedAndPartitionInvariant();
    testBoundedRealtimeCallbackCost();
    testPreparedProcessingAndResetDoNotAllocate();
    testDynamicIntervalsAreFixedLatencyAndPartitionInvariant();
    testDynamicConstantIntervalsAtAcceptedSampleRates();
    testDynamicRejectedControlDoesNotAdvanceState();
    testFrequencyScaleRatioAuthorityIsClosed();
    testKnownPitchRatioAuthorityIsClosed();
    testMonotonizerBlockControlAuthorityIsClosed();
    testActivePitchStartupUsesBoundedSynthesisWindow();
    testEmptyDynamicPitchBlockConsumesNoTargetState();
    testTimeFrequencyCorrectionAuthorityIsClosed();
    testLatencyResetAndFailuresAreClosed();
    return 0;
}
