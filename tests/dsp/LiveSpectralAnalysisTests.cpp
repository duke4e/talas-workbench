#include "talas/dsp/LiveSpectralAnalysis.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
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
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void requireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
        throw std::runtime_error(message);
}

talas::dsp::LiveSpectralAnalysisConfiguration makeConfiguration(
    std::size_t maximumBlockFrames = 64,
    bool fundamentalOnly = true,
    double initialFrequency = 8.0,
    double amplitudeScale = 1.0,
    double frequencyScale = 1.0,
    double lowFrequency = 1.0,
    double highFrequency = 32.0)
{
    const auto result =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            64.0,
            maximumBlockFrames,
            1.0,
            amplitudeScale,
            frequencyScale,
            lowFrequency,
            highFrequency,
            initialFrequency,
            fundamentalOnly,
        });
    require(
        static_cast<bool>(result),
        "defining live spectral configuration should derive");
    return result.configuration;
}

std::vector<double> sine(
    std::size_t size,
    double bin,
    double amplitude,
    std::size_t frameSize = 64)
{
    std::vector<double> result(size);
    for (std::size_t index = 0; index < size; ++index)
    {
        result[index] = amplitude * std::sin(
            2.0 * std::numbers::pi_v<double> * bin
            * static_cast<double>(index)
            / static_cast<double>(frameSize));
    }
    return result;
}

std::vector<double> sumSines(
    std::size_t size,
    std::initializer_list<std::pair<double, double>> tones)
{
    std::vector<double> result(size, 0.0);
    for (const auto [bin, amplitude] : tones)
    {
        const auto tone = sine(size, bin, amplitude);
        for (std::size_t index = 0; index < size; ++index)
            result[index] += tone[index];
    }
    return result;
}

void testConfigurationDerivesExactHannFrameAuthority()
{
    const auto result =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            48000.0,
            17,
            750.0,
            0.5,
            2.0,
            100.0,
            12000.0,
            440.0,
            false,
        });
    require(static_cast<bool>(result),
            "valid analysis configuration should derive");
    require(result.configuration.stft.frameSize == 64
                && result.configuration.stft.hopSize == 16
                && result.configuration.stft.window
                    == talas::dsp::SpectralWindowKind::Hann
                && result.configuration.frameCapacity == 3
                && result.configuration.maximumBlockFrames == 17,
            "configuration should record exact N/H/window/capacity authority");
    const auto justAboveIntegerRatio =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            48000.0,
            17,
            749.9999999999989,
            1.0,
            1.0,
            100.0,
            12000.0,
            440.0,
            false,
        });
    require(static_cast<bool>(justAboveIntegerRatio)
                && justAboveIntegerRatio.configuration.stft.frameSize
                    == 128
                && justAboveIntegerRatio.configuration.stft.hopSize
                    == 32,
            "resolution derivation should apply exact ceil above an integer ratio");
    require(
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            48000.0,
            17,
            11.718749999999972,
            1.0,
            1.0,
            20.0,
            12000.0,
            1000.0,
            false,
        }).status
            == talas::dsp::LiveSpectralAnalysisConfigurationStatus::
                UnsupportedResolution,
        "exact ceil above the N=4096 boundary should reject the configuration");
    require(
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            48000.0, 17, 11.0, 1.0, 1.0,
            20.0, 12000.0, 1000.0, false,
        }).status
            == talas::dsp::LiveSpectralAnalysisConfigurationStatus::
                UnsupportedResolution,
        "resolution beyond N=4096 should fail instead of clamp");
}

void testAnalyzerPublishesCalibratedBinCenteredPeakAndSilence()
{
    const auto configuration = makeConfiguration();
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    const auto input = sine(64, 8.0, 0.75);
    require(analyzer.process(
                input,
                {},
                {},
                {4, 9, 100},
                output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "bin-centered analysis should succeed");
    require(output.frames().size() == 1,
            "one complete Hann frame should publish once");
    const auto& frame = output.frames().front();
    require(frame.sourceTimestamp
                    == talas::dsp::SpectralFrameTimestamp{4, 9, 100}
                && frame.sourceSequence == 0
                && frame.partials.size() == 1,
            "first frame should preserve source time and sequence");
    requireNear(frame.partials.front().frequencyHz, 8.0, 1.0e-9,
                "bin-centered frequency should be exact");
    requireNear(frame.partials.front().amplitude, 0.75, 1.0e-9,
                "periodic Hann coherent-gain amplitude should be calibrated");
    require(std::isfinite(frame.partials.front().bandwidthHz)
                && frame.partials.front().bandwidthHz > 0.0
                && frame.partials.front().bandwidthHz <= 31.0,
            "selected peak bandwidth should be finite and band-bounded");

    analyzer.reset();
    output.reset();
    const std::vector<double> silence(64, 0.0);
    require(analyzer.process(
                silence,
                {},
                {},
                {5, 10, 200},
                output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "silence analysis should succeed");
    require(output.frames().size() == 1
                && output.frames().front().partials.empty(),
            "silence should publish one timestamped empty frame");
}

void testAnalyzerIsInvariantAcrossRightEdgePartitions()
{
    const auto configuration = makeConfiguration();
    const auto input = sine(64, 8.0, 0.5);

    talas::dsp::PreparedLiveSpectralAnalysis whole(configuration);
    talas::dsp::PreparedPartialSpectrumSequence wholeOutput(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    require(whole.process(input, {}, {}, {1, 2, 300}, wholeOutput)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "whole analysis should succeed");

    talas::dsp::PreparedLiveSpectralAnalysis partitioned(configuration);
    talas::dsp::PreparedPartialSpectrumSequence firstOutput(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    talas::dsp::PreparedPartialSpectrumSequence secondOutput(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    require(partitioned.process(
                std::span<const double>{input}.first(63),
                {},
                {},
                {1, 2, 300},
                firstOutput)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && firstOutput.frames().empty(),
            "frame available at the right edge should wait for the next interval");
    require(partitioned.process(
                std::span<const double>{input}.last(1),
                {},
                {},
                {1, 2, 363},
                secondOutput)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "right-edge continuation should succeed");
    require(secondOutput.frames().size() == 1
                && wholeOutput.frames().size() == 1
                && secondOutput.frames().front().sourceTimestamp
                    == wholeOutput.frames().front().sourceTimestamp
                && secondOutput.frames().front().sourceSequence
                    == wholeOutput.frames().front().sourceSequence
                && secondOutput.frames().front().partials.size() == 1,
            "partitioning must not alter frame timestamp, sequence, or count");
    requireNear(
        secondOutput.frames().front().partials.front().frequencyHz,
        wholeOutput.frames().front().partials.front().frequencyHz,
        1.0e-12,
        "partitioning must preserve peak frequency");
    requireNear(
        secondOutput.frames().front().partials.front().amplitude,
        wholeOutput.frames().front().partials.front().amplitude,
        1.0e-12,
        "partitioning must preserve calibrated amplitude");
}

void testAnalyzerRanksMultiplePeaksAndSelectsSeededFundamental()
{
    const auto input = sumSines(64, {{8.0, 0.75}, {16.0, 0.25}});
    auto configuration = makeConfiguration(64, false);
    talas::dsp::PreparedLiveSpectralAnalysis allPeaks(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    require(allPeaks.process(input, {}, {}, {0, 0, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "two-tone multi-peak analysis should succeed");
    require(output.frames().size() == 1
                && output.frames().front().partials.size() == 2,
            "two separated bin-centered tones should publish exactly two retained peaks");
    requireNear(output.frames().front().partials[0].frequencyHz,
                8.0, 1.0e-9,
                "multi-peak output should be frequency ordered");
    requireNear(output.frames().front().partials[0].amplitude,
                0.75, 1.0e-9,
                "strong tone amplitude should be calibrated");
    requireNear(output.frames().front().partials[1].frequencyHz,
                16.0, 1.0e-9,
                "second separated tone frequency should be exact");
    requireNear(output.frames().front().partials[1].amplitude,
                0.25, 1.0e-9,
                "second tone amplitude should be calibrated");

    configuration = makeConfiguration(64, true, 15.0);
    talas::dsp::PreparedLiveSpectralAnalysis fundamental(configuration);
    output.reset();
    require(fundamental.process(input, {}, {}, {0, 0, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && output.frames().front().partials.size() == 1,
            "fundamental-only analysis should publish one seed-guided peak");
    requireNear(output.frames().front().partials.front().frequencyHz,
                16.0, 1.0e-9,
                "fundamental seed should select the nearest log-frequency peak");
}

void testAnalyzerRetainsStrongestSixtyFourAndPublishesNoPeakFrame()
{
    const auto derived =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            4096.0,
            4096,
            1.0,
            1.0,
            1.0,
            1.0,
            2048.0,
            8.0,
            false,
        });
    require(static_cast<bool>(derived)
                && derived.configuration.stft.frameSize == 4096,
            "64-peak ranking fixture should prepare N=4096");
    std::vector<double> input(4096, 0.0);
    for (std::size_t toneIndex = 0; toneIndex < 65; ++toneIndex)
    {
        const auto bin = 8.0
            + 4.0 * static_cast<double>(toneIndex);
        const auto amplitude = 1.0
            - 0.005 * static_cast<double>(toneIndex);
        for (std::size_t sample = 0; sample < input.size(); ++sample)
        {
            input[sample] += amplitude * std::sin(
                2.0 * std::numbers::pi_v<double> * bin
                * static_cast<double>(sample)
                / 4096.0);
        }
    }
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(
        derived.configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        derived.configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    require(analyzer.process(input, {}, {}, {0, 0, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && output.frames().size() == 1
                && output.frames().front().partials.size() == 64,
            "65 ranked peaks should retain exactly the strongest 64");
    requireNear(output.frames().front().partials.front().frequencyHz,
                8.0, 1.0e-9,
                "retained peaks should publish in ascending frequency order");
    requireNear(output.frames().front().partials.back().frequencyHz,
                260.0, 1.0e-9,
                "the weakest 65th peak should be discarded before frequency ordering");

    const auto emptyBand =
        talas::dsp::deriveLiveSpectralAnalysisConfiguration({
            64.0,
            64,
            1.0,
            1.0,
            1.0,
            10.2,
            10.4,
            10.3,
            false,
        });
    require(static_cast<bool>(emptyBand),
            "narrow no-bin analysis band should remain a valid configuration");
    talas::dsp::PreparedLiveSpectralAnalysis noPeak(
        emptyBand.configuration);
    talas::dsp::PreparedPartialSpectrumSequence emptyOutput(
        emptyBand.configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    const auto offBandTone = sine(64, 8.0, 0.5);
    require(noPeak.process(
                offBandTone, {}, {}, {0, 0, 0}, emptyOutput)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && emptyOutput.frames().size() == 1
                && emptyOutput.frames().front().partials.empty(),
            "finite input with no admitted local-maximum bin should publish an empty frame");
}

void testAnalyzerInterpolatesOffBinPeakWithinFrozenBound()
{
    const auto configuration = makeConfiguration();
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    const auto input = sine(64, 8.25, 0.6);
    require(analyzer.process(input, {}, {}, {0, 0, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && output.frames().size() == 1
                && output.frames().front().partials.size() == 1,
            "off-bin defining fixture should publish one peak");
    const auto& peak = output.frames().front().partials.front();
    require(std::abs(peak.frequencyHz - 8.25) <= 0.25,
            "parabolic interpolation should stay within one quarter bin");
    require(std::isfinite(peak.amplitude) && peak.amplitude > 0.0
                && std::isfinite(peak.bandwidthHz)
                && peak.bandwidthHz > 0.0
                && peak.bandwidthHz <= 31.0,
            "off-bin amplitude and bandwidth should remain finite and bounded");
}

void testAnalyzerSamplesDynamicScaleAtEachFrameSourceStart()
{
    const auto configuration = makeConfiguration(80);
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    const auto input = sine(80, 8.0, 0.4);
    std::vector<double> amplitudeScale(80, 7.0);
    std::vector<double> frequencyScale(80, 3.0);
    amplitudeScale[0] = 2.0;
    amplitudeScale[16] = 0.5;
    frequencyScale[0] = 1.5;
    frequencyScale[16] = 2.0;
    require(analyzer.process(
                input,
                amplitudeScale,
                frequencyScale,
                {3, 4, 100},
                output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "dynamic scale analysis should succeed");
    require(output.frames().size() == 2
                && output.frames()[0].sourceTimestamp.absoluteFrame == 100
                && output.frames()[1].sourceTimestamp.absoluteFrame == 116
                && output.frames()[0].sourceSequence == 0
                && output.frames()[1].sourceSequence == 1,
            "two frames should publish exact H=16 timestamps and sequences");
    requireNear(output.frames()[0].partials.front().amplitude,
                0.8, 1.0e-9,
                "first frame should hold its source-start amplitude scale");
    requireNear(output.frames()[0].partials.front().frequencyHz,
                12.0, 1.0e-9,
                "first frame should hold its source-start frequency scale");
    requireNear(output.frames()[1].partials.front().amplitude,
                0.2, 1.0e-9,
                "second frame should sample scale at source offset H");
    requireNear(output.frames()[1].partials.front().frequencyHz,
                16.0, 1.0e-9,
                "second frame should preserve its independent frequency scale");

    const auto splitConfiguration = makeConfiguration(64);
    talas::dsp::PreparedLiveSpectralAnalysis split(splitConfiguration);
    talas::dsp::PreparedPartialSpectrumSequence firstInterval(
        splitConfiguration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    talas::dsp::PreparedPartialSpectrumSequence secondInterval(
        splitConfiguration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    const auto splitInput = sine(64, 8.0, 0.4);
    const std::vector<double> firstAmplitudeScale{2.0};
    const std::vector<double> firstFrequencyScale{1.5};
    const std::vector<double> laterAmplitudeScale(63, 9.0);
    const std::vector<double> laterFrequencyScale(63, 3.0);
    require(split.process(
                std::span<const double>{splitInput}.first(1),
                firstAmplitudeScale,
                firstFrequencyScale,
                {0, 0, 0},
                firstInterval)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && firstInterval.frames().empty(),
            "frame-start scale should be captured before its frame becomes available");
    require(split.process(
                std::span<const double>{splitInput}.last(63),
                laterAmplitudeScale,
                laterFrequencyScale,
                {0, 0, 1},
                secondInterval)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && secondInterval.frames().size() == 1,
            "frame-start scale metadata should survive callback partitioning");
    requireNear(secondInterval.frames().front().partials.front().amplitude,
                0.8, 1.0e-9,
                "partitioned frame should retain its earlier amplitude scale");
    requireNear(secondInterval.frames().front().partials.front().frequencyHz,
                12.0, 1.0e-9,
                "partitioned frame should retain its earlier frequency scale");
}

void testAnalyzerPreservesCadenceAcrossIrregularPartitionsAndUpstreamLatency()
{
    const auto configuration = makeConfiguration(96);
    const auto input = sine(96, 8.0, 0.5);
    talas::dsp::PreparedLiveSpectralAnalysis whole(configuration);
    talas::dsp::PreparedPartialSpectrumSequence expected(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    require(whole.process(input, {}, {}, {7, 8, 1000}, expected)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && expected.frames().size() == 3,
            "96 source samples should publish q=0,1,2 frames");

    talas::dsp::PreparedLiveSpectralAnalysis partitioned(configuration);
    talas::dsp::PreparedPartialSpectrumSequence intervalOutput(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    std::size_t offset = 0;
    std::size_t compared = 0;
    for (const auto size : {7U, 19U, 3U, 21U, 46U})
    {
        intervalOutput.reset();
        require(partitioned.process(
                    std::span<const double>{input}.subspan(offset, size),
                    {},
                    {},
                    {7, 8, 1000 + offset},
                    intervalOutput)
                    == talas::dsp::LiveSpectralAnalysisStatus::Ok,
                "irregular partition should process continuously");
        for (const auto& frame : intervalOutput.frames())
        {
            require(compared < expected.frames().size()
                        && frame.sourceTimestamp
                            == expected.frames()[compared].sourceTimestamp
                        && frame.sourceSequence
                            == expected.frames()[compared].sourceSequence
                        && frame.partials.size()
                            == expected.frames()[compared].partials.size(),
                    "irregular partitions must preserve frame identity and count");
            requireNear(frame.partials.front().frequencyHz,
                        expected.frames()[compared].partials.front().frequencyHz,
                        1.0e-12,
                        "irregular partition should preserve frequency");
            requireNear(frame.partials.front().amplitude,
                        expected.frames()[compared].partials.front().amplitude,
                        1.0e-12,
                        "irregular partition should preserve amplitude");
            ++compared;
        }
        offset += size;
    }
    require(offset == input.size() && compared == expected.frames().size(),
            "irregular partitions should consume and reproduce every expected frame");

    const auto latencyConfiguration = makeConfiguration(69);
    talas::dsp::PreparedLiveSpectralAnalysis delayed(latencyConfiguration);
    talas::dsp::PreparedPartialSpectrumSequence delayedOutput(
        latencyConfiguration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    std::vector<double> delayedInput(69, 0.0);
    const auto source = sine(64, 8.0, 0.4);
    std::ranges::copy(source, delayedInput.begin() + 5);
    std::vector<double> amplitudeScale(69, 1.0);
    std::vector<double> frequencyScale(69, 1.0);
    amplitudeScale[5] = 2.0;
    frequencyScale[5] = 1.5;
    require(delayed.process(
                delayedInput,
                amplitudeScale,
                frequencyScale,
                {11, 12, 500},
                delayedOutput,
                5)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && delayedOutput.frames().size() == 1,
            "upstream startup latency should establish source phase without analysis padding");
    const auto& delayedFrame = delayedOutput.frames().front();
    require(delayedFrame.sourceTimestamp
                    == talas::dsp::SpectralFrameTimestamp{11, 12, 500}
                && delayedFrame.sourceSequence == 0,
            "upstream latency should not shift the published source timestamp");
    requireNear(delayedFrame.partials.front().amplitude,
                0.8, 1.0e-9,
                "upstream phase should sample amplitude scale at the first source sample");
    requireNear(delayedFrame.partials.front().frequencyHz,
                12.0, 1.0e-9,
                "upstream phase should sample frequency scale at the first source sample");
}

void testAnalyzerFailuresArePreflightedAndResetStartsANewSequence()
{
    const auto configuration = makeConfiguration(96);
    const auto input = sine(96, 8.0, 0.5);
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);

    talas::dsp::PreparedPartialSpectrumSequence prefilled(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    require(prefilled.append({0, 0, 0}, 0, {})
                == talas::dsp::SpectralFrameStatus::Ok
                && analyzer.process(input, {}, {}, {1, 1, 0}, prefilled)
                    == talas::dsp::LiveSpectralAnalysisStatus::OutputSequenceNotEmpty,
            "analyzer should reject a non-empty destination before state changes");
    require(analyzer.process(
                input,
                std::span<const double>{input}.first(1),
                {},
                {1, 1, 0},
                output)
                == talas::dsp::LiveSpectralAnalysisStatus::InvalidSpan,
            "connected scale span must match the complete process interval");
    std::vector<double> oversizedInput(97, 0.0);
    require(analyzer.process(
                oversizedInput, {}, {}, {1, 1, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::InputCapacityExceeded,
            "input beyond the prepared callback bound should fail closed");
    require(analyzer.process(
                std::span<const double>{input}.first(64),
                {},
                {},
                {1, 1, std::numeric_limits<std::uint64_t>::max() - 32},
                output)
                == talas::dsp::LiveSpectralAnalysisStatus::TimestampOverflow,
            "process interval timestamp overflow should fail before state changes");

    auto invalidAudio = input;
    invalidAudio[10] = std::numeric_limits<double>::quiet_NaN();
    require(analyzer.process(invalidAudio, {}, {}, {1, 1, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::NonFiniteInput
                && output.frames().empty(),
            "non-finite audio should fail before publishing or advancing state");
    std::vector<double> invalidScale(input.size(), 1.0);
    invalidScale[20] = -1.0;
    require(analyzer.process(input, invalidScale, {}, {1, 1, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::InvalidScale
                && output.frames().empty(),
            "invalid dynamic scale should fail before publishing or advancing state");
    talas::dsp::PreparedPartialSpectrumSequence tooSmall(
        1,
        talas::dsp::maximumLiveSpectralPartials);
    require(analyzer.process(input, {}, {}, {1, 1, 0}, tooSmall)
                == talas::dsp::LiveSpectralAnalysisStatus::OutputCapacityExceeded
                && tooSmall.frames().empty(),
            "insufficient output capacity should fail atomically before STFT state changes");
    talas::dsp::PreparedPartialSpectrumSequence wrongShape(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials - 1);
    require(analyzer.process(input, {}, {}, {1, 1, 0}, wrongShape)
                == talas::dsp::LiveSpectralAnalysisStatus::OutputShapeMismatch,
            "partial output shape mismatch should fail closed");
    require(analyzer.process(input, {}, {}, {1, 1, 0}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && output.frames().size() == 3,
            "valid retry at the same timestamp should prove preflight failure atomicity");

    output.reset();
    require(analyzer.process(
                std::span<const double>{input}.first(1),
                {},
                {},
                {2, 2, 96},
                output)
                == talas::dsp::LiveSpectralAnalysisStatus::DiscontinuousBlock
                && output.frames().empty(),
            "transport discontinuity without an owning reset should fail closed");
    analyzer.reset();
    require(analyzer.process(input, {}, {}, {2, 2, 1000}, output)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && output.frames().front().sourceSequence == 0
                && output.frames().front().sourceTimestamp.absoluteFrame
                    == 1000,
            "reset should clear history and restart source sequence in a new domain");

    const auto overflowConfiguration = makeConfiguration(64);
    talas::dsp::PreparedLiveSpectralAnalysis numericFailure(
        overflowConfiguration);
    talas::dsp::PreparedPartialSpectrumSequence numericOutput(
        overflowConfiguration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);
    std::vector<double> overflowingFrequencyScale(
        64,
        std::numeric_limits<double>::max());
    require(numericFailure.process(
                std::span<const double>{input}.first(64),
                {},
                overflowingFrequencyScale,
                {0, 0, 0},
                numericOutput)
                == talas::dsp::LiveSpectralAnalysisStatus::NumericOverflow
                && numericOutput.frames().empty(),
            "numeric peak overflow should commit no partial frame");
    std::ranges::fill(overflowingFrequencyScale, 1.0);
    require(numericFailure.process(
                std::span<const double>{input}.first(64),
                {},
                overflowingFrequencyScale,
                {0, 0, 0},
                numericOutput)
                == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && numericOutput.frames().size() == 1
                && numericOutput.frames().front().sourceSequence == 0
                && numericOutput.frames().front()
                       .sourceTimestamp.absoluteFrame
                    == 0,
            "post-STFT numeric failure should permit an unchanged-timestamp retry");
}

void testAnalyzerRejectsTamperedPreparedConfiguration()
{
    auto configuration = makeConfiguration();
    ++configuration.frameCapacity;
    bool rejected = false;
    try
    {
        talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    }
    catch (const std::invalid_argument&)
    {
        rejected = true;
    }
    require(rejected,
            "prepared analyzer should reject a self-inconsistent frame capacity");
}

void testAnalyzerProcessAndResetDoNotAllocate()
{
    const auto configuration = makeConfiguration(64);
    const auto input = sine(64, 8.0, 0.5);
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity,
        talas::dsp::maximumLiveSpectralPartials);

    allocationCount.store(0, std::memory_order_relaxed);
    countAllocations.store(true, std::memory_order_release);
    const auto first = analyzer.process(
        input, {}, {}, {0, 0, 0}, output);
    output.reset();
    const auto repeated = analyzer.process(
        input, {}, {}, {0, 0, 64}, output);
    analyzer.reset();
    output.reset();
    countAllocations.store(false, std::memory_order_release);

    require(first == talas::dsp::LiveSpectralAnalysisStatus::Ok
                && repeated == talas::dsp::LiveSpectralAnalysisStatus::Ok,
            "first and repeated prepared analyzer calls should succeed");
    require(allocationCount.load(std::memory_order_relaxed) == 0,
            "prepared analyzer process and reset must not allocate");
}
void testProcessingClonePreservesPendingMetadataAndFrames()
{
    using namespace talas::dsp;
    for (const bool fundamentalOnly : {false, true})
        for (const std::size_t latency : {0, 7})
        {
            const auto configuration = makeConfiguration(17, fundamentalOnly);
            PreparedLiveSpectralAnalysis candidate(configuration), reference(configuration);
            PreparedPartialSpectrumSequence actual(configuration.frameCapacity, maximumLiveSpectralPartials);
            PreparedPartialSpectrumSequence expected(configuration.frameCapacity, maximumLiveSpectralPartials);
            const auto input = sine(512, 8.0, 0.75);
            std::vector<double> amplitudes(input.size()), frequencies(input.size());
            for (std::size_t frame = 0; frame != input.size(); ++frame)
            {
                amplitudes[frame] = 0.25 + static_cast<double>(frame) / 500.0;
                frequencies[frame] = 1.0 + static_cast<double>(frame) / 1000.0;
            }
            const auto process = [&](auto& analyzer, auto& output, std::size_t frame, std::size_t count) {
                output.reset();
                return analyzer.process(std::span<const double>(input).subspan(frame, count),
                    std::span<const double>(amplitudes).subspan(frame, count),
                    std::span<const double>(frequencies).subspan(frame, count),
                    {71, 3, 1000 + frame}, output, latency);
            };
            for (std::size_t frame = 0; frame != 193;)
            {
                const auto count = std::min(std::size_t{17}, 193 - frame);
                require(process(candidate, actual, frame, count) == LiveSpectralAnalysisStatus::Ok
                    && process(reference, expected, frame, count) == LiveSpectralAnalysisStatus::Ok,
                    "analysis clone fixture must prime independent metadata histories");
                frame += count;
            }
            auto baseline = candidate.cloneProcessingState();
            for (std::size_t frame = 193; frame != 312; frame += 17)
                require(process(candidate, actual, frame, 17) == LiveSpectralAnalysisStatus::Ok,
                    "analysis clone fixture must overwrite pending metadata and spectral history");
            allocationCount.store(0);
            countAllocations.store(true);
            const auto restored = candidate.copyProcessingStateFrom(*baseline);
            countAllocations.store(false);
            require(restored && allocationCount.load() == 0,
                "analysis clone must restore without allocation");
            baseline.reset();
            std::size_t verifiedFrames = 0;
            for (std::size_t frame = 193; frame != 417;)
            {
                const auto count = std::min(std::size_t{17}, 417 - frame);
                allocationCount.store(0);
                countAllocations.store(true);
                const auto actualStatus = process(candidate, actual, frame, count);
                const auto expectedStatus = process(reference, expected, frame, count);
                countAllocations.store(false);
                require(actualStatus == LiveSpectralAnalysisStatus::Ok && expectedStatus == LiveSpectralAnalysisStatus::Ok
                    && allocationCount.load() == 0 && actual.frames().size() == expected.frames().size(),
                    "analysis clone continuation must preserve allocation-free frame production");
                for (std::size_t index = 0; index != actual.frames().size(); ++index)
                {
                    const auto& a = actual.frames()[index]; const auto& b = expected.frames()[index];
                    require(a.formatVersion == b.formatVersion && a.sourceTimestamp == b.sourceTimestamp
                        && a.sourceSequence == b.sourceSequence && a.partials.size() == b.partials.size()
                        && !a.partials.empty(), "analysis clone must retain exact frame identities");
                    require(a.sourceTimestamp == SpectralFrameTimestamp{71, 3, 1000 + a.sourceSequence * 16},
                        "restored analysis metadata must retain the independent source-time/sequence relation");
                    const auto sourceInputIndex = a.sourceSequence * 16 + latency;
                    requireNear(a.partials.front().amplitude, 0.75 * amplitudes[sourceInputIndex], 1.0e-9,
                        "restored analysis must apply amplitude captured at the original frame start");
                    requireNear(a.partials.front().frequencyHz, 8.0 * frequencies[sourceInputIndex], 1.0e-9,
                        "restored analysis must apply frequency scale captured at the original frame start");
                    for (std::size_t partial = 0; partial != a.partials.size(); ++partial)
                    {
                        const auto& x = a.partials[partial]; const auto& y = b.partials[partial];
                        require(x.hasTrackId == y.hasTrackId && x.trackId == y.trackId && x.amplitude == y.amplitude
                            && x.frequencyHz == y.frequencyHz && x.bandwidthHz == y.bandwidthHz,
                            "analysis clone must own exact continuation payloads after snapshot destruction");
                    }
                    ++verifiedFrames;
                }
                frame += count;
            }
            require(verifiedFrames > 10, "analysis clone test must cross many frame and metadata-ring boundaries");
        }
}


// A 100-120 Hz band at 48 kHz/N2048 makes (high/bw - low/bw) * bw exceed
// (high - low) by one ulp once both half-power crossings clamp to the band
// edges. That rounding must not fail the whole block.
void testNarrowBandEdgeRoundingDoesNotFailTheBlock()
{
    const auto derived = talas::dsp::deriveLiveSpectralAnalysisConfiguration({
        48000.0, 2048, 25.0, 1.0, 1.0, 100.0, 120.0, 110.0, true});
    require(static_cast<bool>(derived), "narrow band configuration should derive");
    const auto& configuration = derived.configuration;
    const auto frameSize = configuration.stft.frameSize;
    require(frameSize >= 2048 && configuration.maximumBlockFrames == 2048,
            "narrow band configuration should analyze at least 2048 frames");
    talas::dsp::PreparedLiveSpectralAnalysis analyzer(configuration);
    talas::dsp::PreparedPartialSpectrumSequence output(
        configuration.frameCapacity, talas::dsp::maximumLiveSpectralPartials);
    const auto tone = sine(frameSize, 110.0 * static_cast<double>(frameSize) / 48000.0, 0.5, frameSize);
    for (std::size_t offset = 0; offset < frameSize; offset += 2048)
    {
        const auto block = std::span<const double>(tone).subspan(offset, 2048);
        require(analyzer.process(block, {}, {}, {0, offset, 0}, output)
                    == talas::dsp::LiveSpectralAnalysisStatus::Ok,
                "a tone inside a narrow analysis band must analyze without NumericOverflow");
    }
    require(!output.frames().empty() && !output.frames().back().partials.empty(),
            "the narrow band tone should publish a partial");
    require(output.frames().back().partials.front().bandwidthHz <= 20.0 + 1.0e-9,
            "published bandwidth must stay within the analysis band");
}
}

int main()
{
    testNarrowBandEdgeRoundingDoesNotFailTheBlock();
    testProcessingClonePreservesPendingMetadataAndFrames();
    testConfigurationDerivesExactHannFrameAuthority();
    testAnalyzerPublishesCalibratedBinCenteredPeakAndSilence();
    testAnalyzerIsInvariantAcrossRightEdgePartitions();
    testAnalyzerRanksMultiplePeaksAndSelectsSeededFundamental();
    testAnalyzerRetainsStrongestSixtyFourAndPublishesNoPeakFrame();
    testAnalyzerInterpolatesOffBinPeakWithinFrozenBound();
    testAnalyzerSamplesDynamicScaleAtEachFrameSourceStart();
    testAnalyzerPreservesCadenceAcrossIrregularPartitionsAndUpstreamLatency();
    testAnalyzerFailuresArePreflightedAndResetStartsANewSequence();
    testAnalyzerRejectsTamperedPreparedConfiguration();
    testAnalyzerProcessAndResetDoNotAllocate();
    return 0;
}
