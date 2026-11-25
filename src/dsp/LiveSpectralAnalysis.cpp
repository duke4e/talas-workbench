#include "talas/dsp/LiveSpectralAnalysis.h"

#include "talas/dsp/Radix2Fft.h"
#include "talas/dsp/SpectralWindow.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <limits>
#include <numeric>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] LiveSpectralAnalysisConfiguration
validatedConfiguration(LiveSpectralAnalysisConfiguration configuration)
{
    const auto expectedCapacity = preparedSpectralFrameCapacity(
        configuration.maximumBlockFrames,
        configuration.stft.hopSize,
        1,
        0);
    if (!isValidNativeStftConfiguration(configuration.stft)
        || configuration.stft.window != SpectralWindowKind::Hann
        || configuration.responseBins == 0 || configuration.responseBins > 4
        || configuration.stft.hopSize
            != configuration.stft.frameSize / 4
        || !expectedCapacity.has_value()
        || configuration.frameCapacity != *expectedCapacity
        || !std::isfinite(configuration.sampleRate)
        || configuration.sampleRate <= 0.0
        || !std::isfinite(
            configuration.lowestAnalyzedFrequencyHz)
        || configuration.lowestAnalyzedFrequencyHz <= 0.0
        || !std::isfinite(configuration.amplitudeScale)
        || configuration.amplitudeScale < 0.0
        || !std::isfinite(configuration.frequencyScale)
        || configuration.frequencyScale <= 0.0
        || !std::isfinite(configuration.analysisLowFrequencyHz)
        || !std::isfinite(configuration.analysisHighFrequencyHz)
        || configuration.analysisLowFrequencyHz <= 0.0
        || configuration.analysisLowFrequencyHz
            > configuration.analysisHighFrequencyHz
        || configuration.analysisHighFrequencyHz
            > configuration.sampleRate / 2.0
        || !std::isfinite(configuration.initialFrequencyHz)
        || configuration.initialFrequencyHz <= 0.0
        || (configuration.bandTracks
                ? configuration.trackCount == 0
                    || configuration.trackCount > maximumLiveSpectralPartials
                    || configuration.trackCount
                        > configuration.stft.frameSize / 2
                : configuration.trackCount != 0)
        || (configuration.harmonic
            && (!configuration.bandTracks
                || configuration.trackedHarmonic == 0
                || configuration.trackedHarmonic > configuration.trackCount
                || !std::isfinite(configuration.unpitchedThreshold)
                || configuration.unpitchedThreshold < 0.0
                || configuration.unpitchedThreshold > 1.0
                || !(configuration.trackedLowFrequencyHz > 0.0)
                || !(configuration.trackedHighFrequencyHz
                    >= configuration.trackedLowFrequencyHz)
                || configuration.initialFrequencyHz
                    < configuration.trackedLowFrequencyHz
                || configuration.initialFrequencyHz
                    > configuration.trackedHighFrequencyHz)))
    {
        throw std::invalid_argument(
            "Invalid prepared LiveSpectralAnalysis configuration");
    }
    return configuration;
}

[[nodiscard]] std::size_t checkedMetadataCapacity(
    const LiveSpectralAnalysisConfiguration& configuration)
{
    constexpr std::size_t pendingFrameStarts = 4;
    constexpr std::size_t guardFrames = 2;
    if (configuration.frameCapacity
        > std::numeric_limits<std::size_t>::max()
            - pendingFrameStarts - guardFrames)
    {
        throw std::invalid_argument(
            "LiveSpectralAnalysis metadata capacity overflow");
    }
    return configuration.frameCapacity
        + pendingFrameStarts + guardFrames;
}

[[nodiscard]] bool addWithoutOverflow(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result) noexcept
{
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    result = left + right;
    return true;
}

[[nodiscard]] std::uint64_t emittedFrameCount(
    std::uint64_t sampleCount,
    std::size_t frameSize,
    std::size_t hopSize) noexcept
{
    if (sampleCount < frameSize)
        return 0;
    return 1 + (sampleCount - frameSize) / hopSize;
}

[[nodiscard]] LiveSpectralAnalysisStatus mapStftStatus(
    NativeStftStatus status) noexcept
{
    switch (status)
    {
    case NativeStftStatus::Ok:
        return LiveSpectralAnalysisStatus::Ok;
    case NativeStftStatus::DiscontinuousBlock:
        return LiveSpectralAnalysisStatus::DiscontinuousBlock;
    case NativeStftStatus::TimestampOverflow:
        return LiveSpectralAnalysisStatus::TimestampOverflow;
    case NativeStftStatus::NonFiniteInput:
        return LiveSpectralAnalysisStatus::NonFiniteInput;
    case NativeStftStatus::NumericOverflow:
        return LiveSpectralAnalysisStatus::NumericOverflow;
    case NativeStftStatus::OutputCapacityExceeded:
    case NativeStftStatus::InputCapacityExceeded:
        return LiveSpectralAnalysisStatus::OutputCapacityExceeded;
    case NativeStftStatus::OutputSequenceNotEmpty:
    case NativeStftStatus::OutputSequenceRejected:
    case NativeStftStatus::MissingFrame:
    case NativeStftStatus::UnexpectedFrame:
    case NativeStftStatus::InvalidFrame:
    case NativeStftStatus::NonRealEndpoint:
        return LiveSpectralAnalysisStatus::InternalFrameMismatch;
    }
    return LiveSpectralAnalysisStatus::InternalFrameMismatch;
}
}

LiveSpectralAnalysisConfigurationResult
deriveLiveSpectralAnalysisConfiguration(
    const LiveSpectralAnalysisConfigurationInput& input) noexcept
{
    using Status = LiveSpectralAnalysisConfigurationStatus;
    const auto fail = [](Status status) {
        return LiveSpectralAnalysisConfigurationResult{status, {}};
    };

    if (!std::isfinite(input.sampleRate) || input.sampleRate <= 0.0)
        return fail(Status::InvalidSampleRate);
    if (input.maximumBlockFrames == 0)
        return fail(Status::InvalidMaximumBlockFrames);
    if (!std::isfinite(input.lowestAnalyzedFrequencyHz)
        || input.lowestAnalyzedFrequencyHz <= 0.0)
    {
        return fail(Status::InvalidLowestAnalyzedFrequency);
    }
    if (!std::isfinite(input.amplitudeScale)
        || input.amplitudeScale < 0.0)
    {
        return fail(Status::InvalidAmplitudeScale);
    }
    if (!std::isfinite(input.frequencyScale)
        || input.frequencyScale <= 0.0)
    {
        return fail(Status::InvalidFrequencyScale);
    }
    // LowFreq, HighFreq and InitFreq bound the analysis only for the legacy
    // peaks and for a harmonic analysis's tracked harmonic.
    if ((!input.bandTracks || input.harmonic)
        && (!std::isfinite(input.lowFrequencyHz)
            || !std::isfinite(input.highFrequencyHz)
            || input.lowFrequencyHz <= 0.0
            || input.highFrequencyHz <= 0.0
            || input.lowFrequencyHz > input.highFrequencyHz
            || input.lowestAnalyzedFrequencyHz > input.highFrequencyHz))
    {
        return fail(Status::InvalidAnalysisBand);
    }
    if (!std::isfinite(input.initialFrequencyHz)
        || (input.bandTracks && !input.harmonic
                ? input.initialFrequencyHz <= 0.0
                : input.initialFrequencyHz < input.lowFrequencyHz
                    || input.initialFrequencyHz > input.highFrequencyHz))
    {
        return fail(Status::InvalidInitialFrequency);
    }
    if (input.harmonic
        && (!input.bandTracks || input.trackedHarmonic == 0
            || input.trackedHarmonic > maximumLiveSpectralPartials
            || !std::isfinite(input.unpitchedThreshold)
            || input.unpitchedThreshold < 0.0
            || input.unpitchedThreshold > 1.0
            || input.highFrequencyHz >= input.sampleRate / 2.0))
    {
        return fail(Status::InvalidAnalysisBand);
    }

    const auto nyquist = input.sampleRate / 2.0;
    auto analysisLow = input.bandTracks
        ? input.lowestAnalyzedFrequencyHz
        : std::max(
            input.lowFrequencyHz,
            input.lowestAnalyzedFrequencyHz);
    auto analysisHigh = input.bandTracks
        ? nyquist
        : std::min(
            input.highFrequencyHz,
            nyquist);
    if (!std::isfinite(nyquist) || nyquist <= 0.0
        || analysisLow > analysisHigh)
    {
        return fail(Status::InvalidAnalysisBand);
    }

    const auto resolutionRatio =
        static_cast<long double>(input.sampleRate)
        / static_cast<long double>(
            input.lowestAnalyzedFrequencyHz);
    const auto required = std::ceil(resolutionRatio);
    if (!std::isfinite(required) || required < 1.0L
        || required
            > static_cast<long double>(maximumRadix2FftSize))
    {
        return fail(Status::UnsupportedResolution);
    }

    auto frameSize = minimumRadix2FftSize;
    while (static_cast<long double>(frameSize) < required)
        frameSize *= 2;
    // The bands come from the power of two nearest the sample rate over
    // LowestAnalyzedFreq, so 1 F through 5 F give 512, 256, 128, 64 and 32
    // partials at 44.1 and 48 kHz alike.
    const auto nearestPower = input.bandTracks && !input.legacyNextPower;
    if (nearestPower && frameSize > minimumRadix2FftSize
        && static_cast<long double>(frameSize)
            > resolutionRatio * std::sqrt(2.0L))
    {
        frameSize /= 2;
    }
    const auto hopSize = frameSize / 4;
    const auto frameCapacity = preparedSpectralFrameCapacity(
        input.maximumBlockFrames,
        hopSize,
        1,
        0);
    if (!frameCapacity.has_value())
        return fail(Status::FrameCapacityOverflow);

    LiveSpectralAnalysisConfiguration configuration;
    configuration.stft = {
        frameSize,
        hopSize,
        SpectralWindowKind::Hann,
    };
    configuration.frameCapacity = *frameCapacity;
    configuration.maximumBlockFrames = input.maximumBlockFrames;
    configuration.sampleRate = input.sampleRate;
    configuration.lowestAnalyzedFrequencyHz =
        input.lowestAnalyzedFrequencyHz;
    configuration.amplitudeScale = input.amplitudeScale;
    configuration.frequencyScale = input.frequencyScale;
    configuration.analysisLowFrequencyHz = analysisLow;
    configuration.analysisHighFrequencyHz = analysisHigh;
    configuration.initialFrequencyHz = input.initialFrequencyHz;
    configuration.fundamentalOnly = input.fundamentalOnly;
    if (input.responseBins == 0 || input.responseBins > 4)
        return fail(Status::InvalidAnalysisBand);
    configuration.responseBins = input.responseBins;
    if (input.bandTracks)
    {
        // A track for every bin from the first up to half the sample rate,
        // at most 512 of them; before format 75 the last stopped below it.
        configuration.bandTracks = true;
        configuration.trackCount = std::min(
            maximumLiveSpectralPartials,
            nearestPower ? frameSize / 2 : frameSize / 2 - 1);
        configuration.analysisLowFrequencyHz =
            input.sampleRate / static_cast<double>(frameSize);
        configuration.analysisHighFrequencyHz = nyquist;
        // FundamentalOnly applies to a harmonic analysis only.
        configuration.fundamentalOnly =
            input.harmonic && input.fundamentalOnly;
        if (input.harmonic)
        {
            if (input.trackedHarmonic > configuration.trackCount)
                return fail(Status::InvalidAnalysisBand);
            configuration.harmonic = true;
            configuration.trackedHarmonic = input.trackedHarmonic;
            configuration.unpitchedOnly = input.unpitchedOnly;
            configuration.unpitchedThreshold = input.unpitchedThreshold;
            configuration.trackedLowFrequencyHz = input.lowFrequencyHz;
            configuration.trackedHighFrequencyHz = input.highFrequencyHz;
        }
    }
    return {Status::Ok, configuration};
}

PreparedLiveSpectralAnalysis::PreparedLiveSpectralAnalysis(
    LiveSpectralAnalysisConfiguration configuration)
    : configuration_(validatedConfiguration(
          std::move(configuration))),
      stft_(configuration_.stft),
      stftCheckpoint_(configuration_.stft),
      complexFrames_(
          configuration_.frameCapacity,
          configuration_.stft.frameSize / 2 + 1,
          1),
      preflightFrames_(
          configuration_.frameCapacity,
          maximumLiveSpectralPartials),
      window_(makeSpectralWindow(
          SpectralWindowKind::Hann,
          configuration_.stft.frameSize)),
      coherentGain_(
          std::accumulate(window_.begin(), window_.end(), 0.0)
          / static_cast<double>(configuration_.stft.frameSize)),
      responseScratch_(configuration_.responseBins > 1
              ? configuration_.stft.frameSize / 2 + 1
              : 0),
      magnitudeScratch_(configuration_.stft.frameSize / 2 + 1),
      candidateScratch_(configuration_.stft.frameSize / 2 + 1),
      magnitudeHarmonicScratch_(configuration_.stft.frameSize / 2 + 1),
      previousPhases_(configuration_.stft.frameSize / 2 + 1),
      candidatePhases_(configuration_.stft.frameSize / 2 + 1),
      trackedFrequency_(configuration_.initialFrequencyHz),
      candidateTrackedFrequency_(configuration_.initialFrequencyHz),
      metadata_(checkedMetadataCapacity(configuration_)),
      captureScratch_(configuration_.frameCapacity)
{
    if (configuration_.responseBins > 1)
    {
        // Weights cos^2(pi m / 2k) for |m| < k, alternating in sign so the
        // window they make peaks at the frame's centre.
        const auto bins = static_cast<int>(configuration_.responseBins);
        auto sum = 0.0;
        for (int offset = 1 - bins; offset < bins; ++offset)
        {
            const auto weight = std::pow(std::cos(std::numbers::pi * offset / (2.0 * bins)), 2.0);
            responseKernel_.push_back(weight);
            sum += weight;
        }
        for (std::size_t index = 0; index < responseKernel_.size(); ++index)
        {
            const auto offset = static_cast<int>(index) + 1 - bins;
            responseKernel_[index] *= (offset % 2 == 0 ? 1.0 : -1.0) / sum;
        }
        // The window's mean over the frame, as the narrower window it now
        // makes.
        const auto frameSize = static_cast<double>(configuration_.stft.frameSize);
        auto gain = 0.0;
        for (std::size_t sample = 0; sample < window_.size(); ++sample)
        {
            auto taper = 0.0;
            for (std::size_t index = 0; index < responseKernel_.size(); ++index)
            {
                const auto offset = static_cast<double>(static_cast<int>(index) + 1 - bins);
                taper += responseKernel_[index]
                    * std::cos(2.0 * std::numbers::pi * offset * static_cast<double>(sample) / frameSize);
            }
            gain += window_[sample] * taper;
        }
        coherentGain_ = gain / frameSize;
    }
    if (!std::isfinite(coherentGain_) || coherentGain_ <= 0.0)
        throw std::invalid_argument(
            "LiveSpectralAnalysis Hann coherent gain is invalid");
}

LiveSpectralAnalysisStatus PreparedLiveSpectralAnalysis::process(
    std::span<const double> input,
    std::span<const double> amplitudeScale,
    std::span<const double> frequencyScale,
    SpectralFrameTimestamp blockStart,
    PreparedPartialSpectrumSequence& output,
    std::size_t compiledInputLatencyFrames) noexcept
{
    if ((!amplitudeScale.empty()
            && amplitudeScale.size() != input.size())
        || (!frequencyScale.empty()
            && frequencyScale.size() != input.size()))
    {
        return LiveSpectralAnalysisStatus::InvalidSpan;
    }
    if (input.size() > configuration_.maximumBlockFrames)
        return LiveSpectralAnalysisStatus::InputCapacityExceeded;
    if (!output.frames().empty())
        return LiveSpectralAnalysisStatus::OutputSequenceNotEmpty;
    if (output.maximumPartialsPerFrame()
        != maximumLiveSpectralPartials)
    {
        return LiveSpectralAnalysisStatus::OutputShapeMismatch;
    }
    if (initialized_
        && (blockStart.transportEpoch
                != segmentStart_.transportEpoch
            || blockStart.discontinuitySequence
                != segmentStart_.discontinuitySequence
            || blockStart.absoluteFrame != nextBlockAbsoluteFrame_
            || compiledInputLatencyFrames
                != preparedInputLatencyFrames_))
    {
        return LiveSpectralAnalysisStatus::DiscontinuousBlock;
    }

    std::uint64_t blockEnd = 0;
    const auto activeSegmentStart = initialized_
        ? segmentStart_
        : blockStart;
    std::uint64_t phaseStartAbsoluteFrame = 0;
    if (!addWithoutOverflow(
            blockStart.absoluteFrame,
            static_cast<std::uint64_t>(input.size()),
            blockEnd)
        || !addWithoutOverflow(
            activeSegmentStart.absoluteFrame,
            static_cast<std::uint64_t>(
                compiledInputLatencyFrames),
            phaseStartAbsoluteFrame))
    {
        return LiveSpectralAnalysisStatus::TimestampOverflow;
    }

    const auto startupFramesRemaining =
        blockStart.absoluteFrame < phaseStartAbsoluteFrame
        ? phaseStartAbsoluteFrame - blockStart.absoluteFrame
        : std::uint64_t{0};
    const auto sourceInputOffset = startupFramesRemaining
            >= static_cast<std::uint64_t>(input.size())
        ? input.size()
        : static_cast<std::size_t>(startupFramesRemaining);
    const auto sourceInputSize = input.size() - sourceInputOffset;
    std::uint64_t newSourceSampleCount = 0;
    if (!addWithoutOverflow(
            sourceSamplesSinceReset_,
            static_cast<std::uint64_t>(sourceInputSize),
            newSourceSampleCount))
    {
        return LiveSpectralAnalysisStatus::TimestampOverflow;
    }

    const auto safeInputMagnitude =
        std::numeric_limits<double>::max()
        / static_cast<double>(configuration_.stft.frameSize);
    for (std::size_t index = 0; index < input.size(); ++index)
    {
        if (!std::isfinite(input[index]))
            return LiveSpectralAnalysisStatus::NonFiniteInput;
        if (std::abs(input[index]) > safeInputMagnitude)
            return LiveSpectralAnalysisStatus::NumericOverflow;
        const auto amp = amplitudeScale.empty()
            ? configuration_.amplitudeScale
            : amplitudeScale[index];
        const auto freq = frequencyScale.empty()
            ? configuration_.frequencyScale
            : frequencyScale[index];
        if (!std::isfinite(amp) || amp < 0.0
            || !std::isfinite(freq) || freq <= 0.0)
        {
            return LiveSpectralAnalysisStatus::InvalidScale;
        }
    }

    const auto oldFrameCount = emittedFrameCount(
        sourceSamplesSinceReset_,
        configuration_.stft.frameSize,
        configuration_.stft.hopSize);
    const auto newFrameCount = emittedFrameCount(
        newSourceSampleCount,
        configuration_.stft.frameSize,
        configuration_.stft.hopSize);
    const auto framesToEmit = newFrameCount - oldFrameCount;
    if (framesToEmit > output.capacity())
        return LiveSpectralAnalysisStatus::OutputCapacityExceeded;

    auto nextFrameStart = nextFrameStartSourceOffset_;
    auto nextSourceSequence = nextSourceSequence_;
    std::size_t captureCount = 0;
    while (nextFrameStart < newSourceSampleCount)
    {
        if (nextFrameStart < sourceSamplesSinceReset_
            || captureCount >= captureScratch_.size()
            || nextSourceSequence
                == std::numeric_limits<std::uint64_t>::max())
        {
            return LiveSpectralAnalysisStatus::MetadataCapacityExceeded;
        }
        const auto relativeSourceOffset = static_cast<std::size_t>(
            nextFrameStart - sourceSamplesSinceReset_);
        const auto inputIndex = sourceInputOffset
            + relativeSourceOffset;
        if (inputIndex >= input.size())
            return LiveSpectralAnalysisStatus::InternalFrameMismatch;
        std::uint64_t timestamp = 0;
        if (!addWithoutOverflow(
                activeSegmentStart.absoluteFrame,
                nextFrameStart,
                timestamp))
        {
            return LiveSpectralAnalysisStatus::TimestampOverflow;
        }
        captureScratch_[captureCount++] = {
            {
                activeSegmentStart.transportEpoch,
                activeSegmentStart.discontinuitySequence,
                timestamp,
            },
            nextSourceSequence,
            amplitudeScale.empty()
                ? configuration_.amplitudeScale
                : amplitudeScale[inputIndex],
            frequencyScale.empty()
                ? configuration_.frequencyScale
                : frequencyScale[inputIndex],
        };
        if (configuration_.stft.hopSize
            > std::numeric_limits<std::uint64_t>::max()
                - nextFrameStart)
        {
            return LiveSpectralAnalysisStatus::TimestampOverflow;
        }
        nextFrameStart += configuration_.stft.hopSize;
        ++nextSourceSequence;
    }
    if (captureCount > metadata_.size() - metadataSize_)
        return LiveSpectralAnalysisStatus::MetadataCapacityExceeded;

    if (!stftCheckpoint_.copyProcessingStateFrom(stft_))
        return LiveSpectralAnalysisStatus::InternalFrameMismatch;
    const auto metadataHeadBeforeProcess = metadataHead_;
    const auto metadataSizeBeforeProcess = metadataSize_;
    const auto failAfterStft = [&](
        LiveSpectralAnalysisStatus status) noexcept {
        const auto restored = stft_.copyProcessingStateFrom(
            stftCheckpoint_);
        metadataHead_ = metadataHeadBeforeProcess;
        metadataSize_ = metadataSizeBeforeProcess;
        complexFrames_.reset();
        preflightFrames_.reset();
        output.reset();
        return restored
            ? status
            : LiveSpectralAnalysisStatus::InternalFrameMismatch;
    };

    complexFrames_.reset();
    const auto stftStatus = stft_.process(
        input,
        blockStart,
        complexFrames_,
        compiledInputLatencyFrames);
    if (stftStatus != NativeStftStatus::Ok)
        return failAfterStft(mapStftStatus(stftStatus));

    for (std::size_t index = 0; index < captureCount; ++index)
        appendMetadata(captureScratch_[index]);
    if (complexFrames_.frames().size() > metadataSize_)
    {
        return failAfterStft(
            LiveSpectralAnalysisStatus::InternalFrameMismatch);
    }

    preflightFrames_.reset();
    std::ranges::copy(previousPhases_, candidatePhases_.begin());
    candidateFrameTimestamp_ = previousFrameTimestamp_;
    candidateHasFrame_ = hasPreviousFrame_;
    candidateTrackedFrequency_ = trackedFrequency_;
    for (std::size_t index = 0;
         index < complexFrames_.frames().size();
         ++index)
    {
        const auto status = reduceFrame(
            complexFrames_.frames()[index],
            metadataAt(index));
        if (status != LiveSpectralAnalysisStatus::Ok)
            return failAfterStft(status);
    }
    for (const auto& frame : preflightFrames_.frames())
    {
        if (output.append(
                frame.sourceTimestamp,
                frame.sourceSequence,
                frame.partials)
            != SpectralFrameStatus::Ok)
        {
            return failAfterStft(
                LiveSpectralAnalysisStatus::OutputSequenceRejected);
        }
    }

    popMetadata(complexFrames_.frames().size());
    std::swap(previousPhases_, candidatePhases_);
    previousFrameTimestamp_ = candidateFrameTimestamp_;
    hasPreviousFrame_ = candidateHasFrame_;
    trackedFrequency_ = candidateTrackedFrequency_;
    sourceSamplesSinceReset_ = newSourceSampleCount;
    nextFrameStartSourceOffset_ = nextFrameStart;
    nextSourceSequence_ = nextSourceSequence;
    if (!initialized_)
    {
        segmentStart_ = blockStart;
        preparedInputLatencyFrames_ = compiledInputLatencyFrames;
        initialized_ = true;
    }
    nextBlockAbsoluteFrame_ = blockEnd;
    return LiveSpectralAnalysisStatus::Ok;
}

std::unique_ptr<PreparedLiveSpectralAnalysis>
PreparedLiveSpectralAnalysis::cloneProcessingState() const
{
    auto result = std::make_unique<PreparedLiveSpectralAnalysis>(configuration_);
    if (!result->copyProcessingStateFrom(*this))
        throw std::logic_error("Live spectral analysis snapshot has incompatible preparation");
    return result;
}

bool PreparedLiveSpectralAnalysis::copyProcessingStateFrom(
    const PreparedLiveSpectralAnalysis& source) noexcept
{
    if (configuration_ != source.configuration_
        || metadata_.size() != source.metadata_.size())
    {
        return false;
    }
    if (!stft_.copyProcessingStateFrom(source.stft_))
        return false;
    std::ranges::copy(source.metadata_, metadata_.begin());
    metadataHead_ = source.metadataHead_;
    metadataSize_ = source.metadataSize_;
    sourceSamplesSinceReset_ = source.sourceSamplesSinceReset_;
    nextFrameStartSourceOffset_ = source.nextFrameStartSourceOffset_;
    nextSourceSequence_ = source.nextSourceSequence_;
    segmentStart_ = source.segmentStart_;
    nextBlockAbsoluteFrame_ = source.nextBlockAbsoluteFrame_;
    preparedInputLatencyFrames_ = source.preparedInputLatencyFrames_;
    initialized_ = source.initialized_;
    std::ranges::copy(source.previousPhases_, previousPhases_.begin());
    previousFrameTimestamp_ = source.previousFrameTimestamp_;
    hasPreviousFrame_ = source.hasPreviousFrame_;
    trackedFrequency_ = source.trackedFrequency_;
    return true;
}

bool PreparedLiveSpectralAnalysis::retimeNextBlockAbsoluteFrame(
    std::uint64_t absoluteFrame) noexcept
{
    if (!initialized_)
        return true;
    if (absoluteFrame < nextBlockAbsoluteFrame_)
        return false;
    const auto delta = absoluteFrame - nextBlockAbsoluteFrame_;
    if (segmentStart_.absoluteFrame
        > std::numeric_limits<std::uint64_t>::max() - delta)
    {
        return false;
    }
    for (std::size_t offset = 0; offset < metadataSize_; ++offset)
    {
        const auto index = (metadataHead_ + offset) % metadata_.size();
        if (metadata_[index].timestamp.absoluteFrame
            > std::numeric_limits<std::uint64_t>::max() - delta)
        {
            return false;
        }
    }
    if (!stft_.retimeNextBlockAbsoluteFrame(absoluteFrame))
        return false;
    segmentStart_.absoluteFrame += delta;
    for (std::size_t offset = 0; offset < metadataSize_; ++offset)
    {
        const auto index = (metadataHead_ + offset) % metadata_.size();
        metadata_[index].timestamp.absoluteFrame += delta;
    }
    nextBlockAbsoluteFrame_ = absoluteFrame;
    return true;
}

LiveSpectralAnalysisStatus PreparedLiveSpectralAnalysis::reduceFrame(
    const ComplexSpectrumFrame& frame,
    const ScaleMetadata& metadata) noexcept
{
    if (frame.formatVersion != complexSpectrumFormatVersion
        || frame.analysisTimestamp != metadata.timestamp
        || frame.sourceSequence != metadata.sourceSequence
        || frame.frameSize != configuration_.stft.frameSize
        || frame.hopSize != configuration_.stft.hopSize
        || frame.window != SpectralWindowKind::Hann
        || frame.channelIndex != 0
        || frame.bins.size() != magnitudeScratch_.size())
    {
        return LiveSpectralAnalysisStatus::InternalFrameMismatch;
    }

    // A Response wider than one bin narrows the window: each bin
    // takes its neighbours, mirrored about 0 hz and half the sample rate.
    auto bins = frame.bins;
    if (!responseScratch_.empty())
    {
        const auto count = static_cast<long>(frame.bins.size());
        const auto reach = static_cast<long>(configuration_.responseBins) - 1;
        for (long bin = 0; bin < count; ++bin)
        {
            std::complex<double> sum{};
            for (long offset = -reach; offset <= reach; ++offset)
            {
                auto source = bin - offset;
                auto mirrored = false;
                if (source < 0)
                {
                    source = -source;
                    mirrored = true;
                }
                if (source >= count)
                {
                    source = 2 * (count - 1) - source;
                    mirrored = true;
                }
                const auto value = frame.bins[static_cast<std::size_t>(source)];
                sum += responseKernel_[static_cast<std::size_t>(offset + reach)]
                    * (mirrored ? std::conj(value) : value);
            }
            responseScratch_[static_cast<std::size_t>(bin)] = sum;
        }
        bins = responseScratch_;
    }

    for (std::size_t bin = 0; bin < bins.size(); ++bin)
    {
        magnitudeScratch_[bin] = std::abs(bins[bin]);
        if (!std::isfinite(magnitudeScratch_[bin]))
            return LiveSpectralAnalysisStatus::NumericOverflow;
    }

    const auto binWidth = configuration_.sampleRate
        / static_cast<double>(configuration_.stft.frameSize);
    // A frame one hop after the last one on the same timeline.
    const auto followsPreviousFrame = candidateHasFrame_
        && frame.analysisTimestamp.transportEpoch
            == candidateFrameTimestamp_.transportEpoch
        && frame.analysisTimestamp.discontinuitySequence
            == candidateFrameTimestamp_.discontinuitySequence
        && frame.analysisTimestamp.absoluteFrame
            == candidateFrameTimestamp_.absoluteFrame
                + configuration_.stft.hopSize;
    const auto maximumMagnitude = *std::ranges::max_element(
        magnitudeScratch_);
    const auto numericNoiseFloor = maximumMagnitude
        * 64.0 * std::numeric_limits<double>::epsilon()
        * static_cast<double>(configuration_.stft.frameSize);
    if (configuration_.bandTracks)
    {
        // Track k follows a peak in bin k + 1 and is silent at the bin's
        // own frequency otherwise, so every track keeps its band.
        const auto nyquist = configuration_.sampleRate * 0.5;
        const auto hopCycles = static_cast<double>(configuration_.stft.hopSize)
            / static_cast<double>(configuration_.stft.frameSize);
        for (std::size_t track = 0; track < configuration_.trackCount; ++track)
        {
            const auto bin = track + 1;
            const auto center = magnitudeScratch_[bin];
            const auto left = magnitudeScratch_[bin - 1];
            // The spectrum mirrors itself about half the sample rate.
            const auto right = bin + 1 < magnitudeScratch_.size()
                ? magnitudeScratch_[bin + 1]
                : left;
            auto point = PartialSpectrumPoint{
                false,
                0,
                0.0,
                static_cast<double>(bin) * binWidth * metadata.frequencyScale,
                binWidth * metadata.frequencyScale,
            };
            if (center > numericNoiseFloor && center > left && center >= right)
            {
                double interpolation = 0.0;
                const auto denominator = left - 2.0 * center + right;
                if (denominator != 0.0)
                {
                    interpolation = std::clamp(
                        0.5 * (left - right) / denominator, -0.5, 0.5);
                }
                const auto peakMagnitude = center
                    - 0.25 * (left - right) * interpolation;
                auto binPosition = static_cast<double>(bin) + interpolation;
                if (followsPreviousFrame)
                {
                    const auto deviation = std::remainder(
                        std::arg(bins[bin]) - candidatePhases_[bin]
                            - 2.0 * std::numbers::pi * static_cast<double>(bin) * hopCycles,
                        2.0 * std::numbers::pi);
                    const auto refined = static_cast<double>(bin)
                        + deviation / (2.0 * std::numbers::pi * hopCycles);
                    if (std::isfinite(refined) && std::abs(refined - binPosition) <= 1.0)
                        binPosition = refined;
                }
                point.amplitude = peakMagnitude * 2.0
                    / (static_cast<double>(configuration_.stft.frameSize) * coherentGain_)
                    * metadata.amplitudeScale;
                point.frequencyHz = binPosition * binWidth * metadata.frequencyScale;
            }
            if (!std::isfinite(point.amplitude) || point.amplitude < 0.0
                || !std::isfinite(point.frequencyHz) || point.frequencyHz <= 0.0
                || !std::isfinite(point.bandwidthHz))
            {
                return LiveSpectralAnalysisStatus::NumericOverflow;
            }
            // A track scaled to or past half the sample rate is silent.
            if (point.frequencyHz >= nyquist)
                point.amplitude = 0.0;
            candidateScratch_[track] = point;
        }
        if (configuration_.harmonic
            && !reduceHarmonics(metadata, binWidth))
        {
            return LiveSpectralAnalysisStatus::NumericOverflow;
        }
        if (preflightFrames_.append(
                metadata.timestamp,
                metadata.sourceSequence,
                std::span<const PartialSpectrumPoint>{candidateScratch_}.first(
                    configuration_.trackCount))
            != SpectralFrameStatus::Ok)
        {
            return LiveSpectralAnalysisStatus::OutputSequenceRejected;
        }
        for (std::size_t bin = 0; bin < bins.size(); ++bin)
            candidatePhases_[bin] = std::arg(bins[bin]);
        candidateFrameTimestamp_ = frame.analysisTimestamp;
        candidateHasFrame_ = true;
        return LiveSpectralAnalysisStatus::Ok;
    }
    std::size_t candidateCount = 0;
    for (std::size_t bin = 0; bin < magnitudeScratch_.size(); ++bin)
    {
        const auto centerFrequency =
            static_cast<double>(bin) * binWidth;
        if (centerFrequency
                < configuration_.analysisLowFrequencyHz
            || centerFrequency
                > configuration_.analysisHighFrequencyHz)
        {
            continue;
        }
        const auto center = magnitudeScratch_[bin];
        const auto left = bin == 0
            ? 0.0
            : magnitudeScratch_[bin - 1];
        const auto right = bin + 1 == magnitudeScratch_.size()
            ? 0.0
            : magnitudeScratch_[bin + 1];
        if (!(center > numericNoiseFloor
                && center > left && center >= right))
            continue;

        double interpolation = 0.0;
        if (bin > 0 && bin + 1 < magnitudeScratch_.size())
        {
            const auto denominator = left - 2.0 * center + right;
            if (denominator != 0.0)
            {
                interpolation = std::clamp(
                    0.5 * (left - right) / denominator,
                    -0.5,
                    0.5);
            }
        }
        const auto peakMagnitude = center
            - 0.25 * (left - right) * interpolation;
        const auto realSpectrumFactor =
            bin == 0 || bin + 1 == magnitudeScratch_.size()
            ? 1.0
            : 2.0;
        const auto amplitude = peakMagnitude
            * realSpectrumFactor
            / (static_cast<double>(configuration_.stft.frameSize)
                * coherentGain_)
            * metadata.amplitudeScale;
        // The peak's phase advance over the hop, less the advance of the
        // bin's own frequency, places it between bins; a result that
        // strays from the magnitude peak is interference and is ignored.
        auto binPosition = static_cast<double>(bin) + interpolation;
        if (followsPreviousFrame)
        {
            const auto hopCycles = static_cast<double>(configuration_.stft.hopSize)
                / static_cast<double>(configuration_.stft.frameSize);
            const auto deviation = std::remainder(
                std::arg(bins[bin]) - candidatePhases_[bin]
                    - 2.0 * std::numbers::pi * static_cast<double>(bin) * hopCycles,
                2.0 * std::numbers::pi);
            const auto refined = static_cast<double>(bin)
                + deviation / (2.0 * std::numbers::pi * hopCycles);
            if (std::isfinite(refined) && std::abs(refined - binPosition) <= 1.0)
                binPosition = refined;
        }
        const auto frequency =
            binPosition * binWidth * metadata.frequencyScale;
        if (!std::isfinite(peakMagnitude)
            || peakMagnitude <= 0.0
            || !std::isfinite(amplitude) || amplitude < 0.0
            || !std::isfinite(frequency) || frequency <= 0.0)
        {
            return LiveSpectralAnalysisStatus::NumericOverflow;
        }

        const auto halfPowerMagnitude = peakMagnitude
            / std::sqrt(2.0);
        const auto lowBinBoundary =
            configuration_.analysisLowFrequencyHz / binWidth;
        const auto highBinBoundary =
            configuration_.analysisHighFrequencyHz / binWidth;
        std::size_t leftInner = bin;
        while (leftInner > 0
            && static_cast<double>(leftInner - 1)
                >= lowBinBoundary
            && magnitudeScratch_[leftInner - 1]
                > halfPowerMagnitude)
        {
            --leftInner;
        }
        double leftCrossing = lowBinBoundary;
        if (leftInner > 0
            && static_cast<double>(leftInner - 1)
                >= lowBinBoundary)
        {
            const auto outer = magnitudeScratch_[leftInner - 1];
            const auto inner = magnitudeScratch_[leftInner];
            const auto denominator = inner - outer;
            leftCrossing = denominator > 0.0
                ? static_cast<double>(leftInner - 1)
                    + std::clamp(
                        (halfPowerMagnitude - outer)
                            / denominator,
                        0.0,
                        1.0)
                : static_cast<double>(leftInner);
        }

        std::size_t rightInner = bin;
        while (rightInner + 1 < magnitudeScratch_.size()
            && static_cast<double>(rightInner + 1)
                <= highBinBoundary
            && magnitudeScratch_[rightInner + 1]
                > halfPowerMagnitude)
        {
            ++rightInner;
        }
        double rightCrossing = highBinBoundary;
        if (rightInner + 1 < magnitudeScratch_.size()
            && static_cast<double>(rightInner + 1)
                <= highBinBoundary)
        {
            const auto inner = magnitudeScratch_[rightInner];
            const auto outer = magnitudeScratch_[rightInner + 1];
            const auto denominator = inner - outer;
            rightCrossing = denominator > 0.0
                ? static_cast<double>(rightInner)
                    + std::clamp(
                        (inner - halfPowerMagnitude)
                            / denominator,
                        0.0,
                        1.0)
                : static_cast<double>(rightInner);
        }
        const auto maximumBandwidth =
            (configuration_.analysisHighFrequencyHz
                - configuration_.analysisLowFrequencyHz)
            * metadata.frequencyScale;
        // Both crossings clamp to the band edges, so rounding can push the
        // product a few ulp past the exact band width; clamp instead of
        // failing the block.
        const auto bandwidth = std::min(
            std::max(0.0, (rightCrossing - leftCrossing) * binWidth)
                * metadata.frequencyScale,
            maximumBandwidth);
        if (!std::isfinite(bandwidth) || bandwidth < 0.0)
            return LiveSpectralAnalysisStatus::NumericOverflow;
        candidateScratch_[candidateCount++] = {
            false,
            0,
            amplitude,
            frequency,
            bandwidth,
        };
    }

    auto candidates = std::span<PartialSpectrumPoint>{
        candidateScratch_}.first(candidateCount);
    std::ranges::sort(candidates, [](const auto& left, const auto& right) {
        if (left.amplitude != right.amplitude)
            return left.amplitude > right.amplitude;
        return left.frequencyHz < right.frequencyHz;
    });
    const auto retainedCount = std::min(
        candidateCount,
        legacyLiveSpectralPartials);
    auto retained = candidates.first(retainedCount);

    if (configuration_.fundamentalOnly && !retained.empty())
    {
        auto selected = retained.begin();
        auto selectedDistance = std::abs(std::log(
            selected->frequencyHz
            / configuration_.initialFrequencyHz));
        for (auto candidate = retained.begin() + 1;
             candidate != retained.end();
             ++candidate)
        {
            const auto distance = std::abs(std::log(
                candidate->frequencyHz
                / configuration_.initialFrequencyHz));
            if (distance < selectedDistance
                || (distance == selectedDistance
                    && candidate->frequencyHz
                        < selected->frequencyHz))
            {
                selected = candidate;
                selectedDistance = distance;
            }
        }
        candidateScratch_.front() = *selected;
        retained = std::span<PartialSpectrumPoint>{
            candidateScratch_}.first(1);
    }
    else
    {
        std::ranges::sort(
            retained,
            {},
            &PartialSpectrumPoint::frequencyHz);
    }

    if (preflightFrames_.append(
            metadata.timestamp,
            metadata.sourceSequence,
            retained)
        != SpectralFrameStatus::Ok)
    {
        return LiveSpectralAnalysisStatus::OutputSequenceRejected;
    }
    for (std::size_t bin = 0; bin < bins.size(); ++bin)
        candidatePhases_[bin] = std::arg(bins[bin]);
    candidateFrameTimestamp_ = frame.analysisTimestamp;
    candidateHasFrame_ = true;
    return LiveSpectralAnalysisStatus::Ok;
}

// The harmonic analysis follows TrackedHarmonic between LowFreq and
// HighFreq: the strongest band peak there, divided by
// TrackedHarmonic, is the fundamental, kept while no peak is found. Track n
// is harmonic n, as loud as the spectrum's peak at it. FundamentalOnly
// sounds the first alone, as loud as the tracked harmonic so the estimate
// can be heard however weak the fundamental is. UnpitchedOnly sounds the
// band peaks only while the harmonics carry less than UnpitchedThreshold
// of the energy.
bool PreparedLiveSpectralAnalysis::reduceHarmonics(
    const ScaleMetadata& metadata,
    double binWidth) noexcept
{
    const auto trackCount = configuration_.trackCount;
    const auto unscaled = [&](std::size_t track) {
        return candidateScratch_[track].frequencyHz / metadata.frequencyScale;
    };
    // The tracked harmonic is the strongest peak between LowFreq and
    // HighFreq.
    auto strongest = 0.0;
    for (std::size_t track = 0; track < trackCount; ++track)
    {
        const auto frequency = unscaled(track);
        if (candidateScratch_[track].amplitude > strongest
            && frequency >= configuration_.trackedLowFrequencyHz
            && frequency <= configuration_.trackedHighFrequencyHz)
        {
            strongest = candidateScratch_[track].amplitude;
            candidateTrackedFrequency_ = frequency;
        }
    }
    const auto fundamental = candidateTrackedFrequency_
        / static_cast<double>(configuration_.trackedHarmonic);
    if (!std::isfinite(fundamental) || fundamental <= 0.0)
        return false;

    // Each harmonic takes the magnitude peak within half a bin of it.
    const auto nyquist = configuration_.sampleRate * 0.5;
    const auto amplitudeScale = 2.0
        / (static_cast<double>(configuration_.stft.frameSize) * coherentGain_)
        * metadata.amplitudeScale;
    auto harmonicEnergy = 0.0;
    auto totalEnergy = 0.0;
    for (std::size_t track = 0; track < trackCount; ++track)
        totalEnergy += candidateScratch_[track].amplitude * candidateScratch_[track].amplitude;
    for (std::size_t track = 0; track < trackCount; ++track)
    {
        const auto harmonicFrequency = fundamental * static_cast<double>(track + 1);
        auto& point = magnitudeHarmonicScratch_[track];
        point = PartialSpectrumPoint{
            false, 0, 0.0,
            harmonicFrequency * metadata.frequencyScale,
            binWidth * metadata.frequencyScale,
        };
        const auto position = harmonicFrequency / binWidth;
        if (harmonicFrequency >= nyquist || position + 1.0
                >= static_cast<double>(magnitudeScratch_.size()))
        {
            continue;
        }
        const auto bin = static_cast<std::size_t>(std::lround(position));
        if (bin == 0)
            continue;
        const auto left = magnitudeScratch_[bin - 1];
        const auto center = magnitudeScratch_[bin];
        const auto right = magnitudeScratch_[bin + 1];
        auto peak = center;
        const auto denominator = left - 2.0 * center + right;
        if (center >= left && center >= right && denominator != 0.0)
        {
            const auto offset = std::clamp(
                0.5 * (left - right) / denominator, -0.5, 0.5);
            peak = center - 0.25 * (left - right) * offset;
        }
        point.amplitude = peak * amplitudeScale;
        if (!std::isfinite(point.amplitude) || point.amplitude < 0.0
            || !std::isfinite(point.frequencyHz))
        {
            return false;
        }
        harmonicEnergy += point.amplitude * point.amplitude;
        if (point.frequencyHz >= nyquist)
            point.amplitude = 0.0;
    }
    const auto pitched = totalEnergy > 0.0
        && harmonicEnergy >= configuration_.unpitchedThreshold * totalEnergy;
    if (configuration_.unpitchedOnly)
    {
        // The band peaks sound only while the frame is unpitched.
        if (pitched)
        {
            for (std::size_t track = 0; track < trackCount; ++track)
                candidateScratch_[track].amplitude = 0.0;
        }
        return true;
    }
    const auto trackedAmplitude =
        magnitudeHarmonicScratch_[configuration_.trackedHarmonic - 1].amplitude;
    for (std::size_t track = 0; track < trackCount; ++track)
    {
        candidateScratch_[track] = magnitudeHarmonicScratch_[track];
        if (configuration_.fundamentalOnly)
            candidateScratch_[track].amplitude = track == 0 ? trackedAmplitude : 0.0;
    }
    return true;
}

const PreparedLiveSpectralAnalysis::ScaleMetadata&
PreparedLiveSpectralAnalysis::metadataAt(
    std::size_t offset) const noexcept
{
    return metadata_[
        (metadataHead_ + offset) % metadata_.size()];
}

void PreparedLiveSpectralAnalysis::appendMetadata(
    const ScaleMetadata& metadata) noexcept
{
    metadata_[
        (metadataHead_ + metadataSize_) % metadata_.size()]
        = metadata;
    ++metadataSize_;
}

void PreparedLiveSpectralAnalysis::popMetadata(
    std::size_t count) noexcept
{
    metadataHead_ = (metadataHead_ + count) % metadata_.size();
    metadataSize_ -= count;
}

void PreparedLiveSpectralAnalysis::reset() noexcept
{
    stft_.reset();
    stftCheckpoint_.reset();
    complexFrames_.reset();
    preflightFrames_.reset();
    std::ranges::fill(magnitudeScratch_, 0.0);
    std::ranges::fill(candidateScratch_, PartialSpectrumPoint{});
    std::ranges::fill(metadata_, ScaleMetadata{});
    std::ranges::fill(captureScratch_, ScaleMetadata{});
    metadataHead_ = 0;
    metadataSize_ = 0;
    sourceSamplesSinceReset_ = 0;
    nextFrameStartSourceOffset_ = 0;
    nextSourceSequence_ = 0;
    segmentStart_ = {};
    nextBlockAbsoluteFrame_ = 0;
    preparedInputLatencyFrames_ = 0;
    initialized_ = false;
    std::ranges::fill(previousPhases_, 0.0);
    std::ranges::fill(candidatePhases_, 0.0);
    previousFrameTimestamp_ = {};
    candidateFrameTimestamp_ = {};
    hasPreviousFrame_ = false;
    candidateHasFrame_ = false;
    trackedFrequency_ = configuration_.initialFrequencyHz;
    candidateTrackedFrequency_ = configuration_.initialFrequencyHz;
}

std::optional<std::uint64_t>
PreparedLiveSpectralAnalysis::nextFrameCompletionFrame(
    std::uint64_t nextBlockStartFrame,
    std::size_t compiledInputLatencyFrames) const noexcept
{
    // Samples count from the segment's start plus the input latency; the
    // next frame is complete once frameSize + k * hop of them are in.
    const auto frameSize =
        static_cast<std::uint64_t>(configuration_.stft.frameSize);
    const auto hopSize =
        static_cast<std::uint64_t>(configuration_.stft.hopSize);
    const auto emitted = emittedFrameCount(
        sourceSamplesSinceReset_,
        configuration_.stft.frameSize,
        configuration_.stft.hopSize);
    std::uint64_t phaseStart = 0;
    std::uint64_t result = 0;
    if (hopSize == 0 || frameSize == 0
        || emitted > (std::numeric_limits<std::uint64_t>::max() - frameSize) / hopSize
        || !addWithoutOverflow(
            initialized_ ? segmentStart_.absoluteFrame : nextBlockStartFrame,
            initialized_ ? preparedInputLatencyFrames_ : compiledInputLatencyFrames,
            phaseStart)
        || !addWithoutOverflow(phaseStart, frameSize + emitted * hopSize - 1, result))
    {
        return std::nullopt;
    }
    return result;
}

const LiveSpectralAnalysisConfiguration&
PreparedLiveSpectralAnalysis::configuration() const noexcept
{
    return configuration_;
}
}
