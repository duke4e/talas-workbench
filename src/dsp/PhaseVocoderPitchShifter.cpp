#include "talas/dsp/PhaseVocoderPitchShifter.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
constexpr NativeStftConfiguration pitchConfiguration{
    phaseVocoderPitchFrameSize,
    phaseVocoderPitchHopSize,
    SpectralWindowKind::Hann,
};

[[nodiscard]] std::size_t validatedFrameCapacity(
    std::size_t maximumBlockFrames)
{
    const auto capacity = preparedSpectralFrameCapacity(
        maximumBlockFrames,
        phaseVocoderPitchHopSize,
        1,
        0);
    if (!capacity.has_value())
        throw std::invalid_argument("Invalid phase-vocoder block capacity");
    return *capacity;
}

[[nodiscard]] double validatedSampleRate(double sampleRate)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        throw std::invalid_argument("Invalid phase-vocoder sample rate");
    return sampleRate;
}

[[nodiscard]] double validatedInterval(double intervalSemitones,
    PhaseVocoderPitchProfile profile)
{
    if (!isSupportedPhaseVocoderPitchInterval(intervalSemitones, profile))
    {
        throw std::invalid_argument(
            "Prepared phase-vocoder interval must be supported");
    }
    return intervalSemitones;
}

[[nodiscard]] PhaseVocoderPitchStatus translateStatus(
    NativeStftStatus status) noexcept
{
    switch (status)
    {
    case NativeStftStatus::Ok:
        return PhaseVocoderPitchStatus::Ok;
    case NativeStftStatus::DiscontinuousBlock:
        return PhaseVocoderPitchStatus::DiscontinuousBlock;
    case NativeStftStatus::TimestampOverflow:
        return PhaseVocoderPitchStatus::TimestampOverflow;
    case NativeStftStatus::NonFiniteInput:
        return PhaseVocoderPitchStatus::NonFiniteInput;
    case NativeStftStatus::NumericOverflow:
        return PhaseVocoderPitchStatus::NumericOverflow;
    case NativeStftStatus::OutputCapacityExceeded:
    case NativeStftStatus::InputCapacityExceeded:
        return PhaseVocoderPitchStatus::BlockCapacityExceeded;
    case NativeStftStatus::OutputSequenceNotEmpty:
    case NativeStftStatus::OutputSequenceRejected:
    case NativeStftStatus::MissingFrame:
    case NativeStftStatus::UnexpectedFrame:
    case NativeStftStatus::InvalidFrame:
    case NativeStftStatus::NonRealEndpoint:
        return PhaseVocoderPitchStatus::InternalSpectralFailure;
    }
    return PhaseVocoderPitchStatus::InternalSpectralFailure;
}

[[nodiscard]] double wrappedPhase(double phase) noexcept
{
    return std::remainder(phase, 2.0 * std::numbers::pi);
}

[[nodiscard]] double interpolatePhase(
    double first,
    double second,
    double fraction) noexcept
{
    return first + fraction * wrappedPhase(second - first);
}
}

bool isSupportedPhaseVocoderPitchInterval(
    double intervalSemitones, PhaseVocoderPitchProfile profile) noexcept
{
    const auto maximum = profile == PhaseVocoderPitchProfile::OneOctave
        ? maximumPhaseVocoderPitchInterval : 24.0;
    return std::isfinite(intervalSemitones)
        && (profile == PhaseVocoderPitchProfile::OneOctave
            || profile == PhaseVocoderPitchProfile::TimeFrequencyCorrection)
        && intervalSemitones >= minimumPhaseVocoderPitchInterval
        && intervalSemitones <= maximum;
}

double phaseVocoderPitchRatio(double intervalSemitones,
    PhaseVocoderPitchProfile profile) noexcept
{
    if (!isSupportedPhaseVocoderPitchInterval(intervalSemitones, profile))
        return std::numeric_limits<double>::quiet_NaN();
    return std::exp2(intervalSemitones / 12.0);
}

bool isSupportedPhaseVocoderFrequencyScale(double frequencyScale) noexcept
{
    return std::isfinite(frequencyScale)
        && frequencyScale >= minimumPhaseVocoderFrequencyScale
        && frequencyScale <= maximumPhaseVocoderFrequencyScale;
}

double phaseVocoderFrequencyScaleInterval(double frequencyScale) noexcept
{
    if (!isSupportedPhaseVocoderFrequencyScale(frequencyScale))
        return std::numeric_limits<double>::quiet_NaN();
    return 12.0 * std::log2(frequencyScale);
}

double phaseVocoderKnownPitchRatio(
    double targetFrequency,
    double minimumInputPitch,
    double maximumInputPitch) noexcept
{
    if (!std::isfinite(targetFrequency) || targetFrequency <= 0.0
        || !std::isfinite(minimumInputPitch) || minimumInputPitch <= 0.0
        || maximumInputPitch != minimumInputPitch)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto ratio = targetFrequency / minimumInputPitch;
    return isSupportedPhaseVocoderFrequencyScale(ratio)
        ? ratio
        : std::numeric_limits<double>::quiet_NaN();
}

double phaseVocoderTimeFrequencyCorrectionRatio(
    double frequencyScale,
    double playbackRate) noexcept
{
    if (!std::isfinite(frequencyScale) || frequencyScale <= 0.0
        || !std::isfinite(playbackRate) || playbackRate <= 0.0)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const auto ratio = frequencyScale / playbackRate;
    return std::isfinite(ratio) && ratio >= minimumPhaseVocoderFrequencyScale
            && ratio <= maximumPhaseVocoderTimeFrequencyCorrection
        ? ratio
        : std::numeric_limits<double>::quiet_NaN();
}

double phaseVocoderTimeFrequencyCorrectionInterval(double correctionRatio) noexcept
{
    if (!std::isfinite(correctionRatio)
        || correctionRatio < minimumPhaseVocoderFrequencyScale
        || correctionRatio > maximumPhaseVocoderTimeFrequencyCorrection)
        return std::numeric_limits<double>::quiet_NaN();
    return 12.0 * std::log2(correctionRatio);
}

MonotonizerControlStatus checkedMonotonizerBlockDecision(
    double sampleRate,
    std::size_t frameCount,
    bool activeProfile,
    MonotonizerControlInput frequency,
    MonotonizerControlInput minimumInputPitch,
    MonotonizerControlInput maximumInputPitch,
    MonotonizerBlockDecision& decision) noexcept
{
    decision = {};
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0)
        return MonotonizerControlStatus::InvalidSampleRate;
    const auto hasExactShape = [&](MonotonizerControlInput input) {
        return !input.connected
            ? input.connectedValues.empty()
            : input.connectedValues.size() == frameCount;
    };
    if (!hasExactShape(frequency)
        || !hasExactShape(minimumInputPitch)
        || !hasExactShape(maximumInputPitch))
    {
        return MonotonizerControlStatus::InvalidBufferShape;
    }
    if (!activeProfile
        && (frequency.connected
            || minimumInputPitch.connected
            || maximumInputPitch.connected))
    {
        return MonotonizerControlStatus::ProfileMismatch;
    }

    if (frameCount == 0)
    {
        decision.activeProfile = activeProfile;
        decision.ratio = 1.0;
        decision.intervalSemitones = 0.0;
        return MonotonizerControlStatus::Ok;
    }

    const auto selected = [](MonotonizerControlInput input,
                             std::size_t frame) {
        return input.connected ? input.connectedValues[frame]
                               : input.fallback;
    };
    const auto validateFrame = [&](double frequencyValue,
                                   double minimumValue,
                                   double maximumValue,
                                   double firstFrequency,
                                   double firstMinimum,
                                   double firstMaximum,
                                   bool compareConstant) {
        if (!std::isfinite(frequencyValue) || frequencyValue <= 0.0)
            return MonotonizerControlStatus::NonPositiveFrequency;
        if (!std::isfinite(minimumValue) || minimumValue <= 0.0)
        {
            return MonotonizerControlStatus::
                NonPositiveMinimumInputPitch;
        }
        if (!std::isfinite(maximumValue) || maximumValue <= 0.0)
        {
            return MonotonizerControlStatus::
                NonPositiveMaximumInputPitch;
        }
        if (minimumValue != maximumValue)
            return MonotonizerControlStatus::NonSingletonInputPitch;
        const auto ratio = phaseVocoderKnownPitchRatio(
            frequencyValue, minimumValue, maximumValue);
        if (!std::isfinite(ratio))
            return MonotonizerControlStatus::UnsupportedRatio;
        if (ratio != 1.0
            && (frequencyValue >= sampleRate * 0.5
                || minimumValue >= sampleRate * 0.5))
        {
            return MonotonizerControlStatus::NyquistExceeded;
        }
        if (compareConstant)
        {
            if (frequency.connected && frequencyValue != firstFrequency)
            {
                return MonotonizerControlStatus::
                    FrequencyNotBlockConstant;
            }
            if (minimumInputPitch.connected
                && minimumValue != firstMinimum)
            {
                return MonotonizerControlStatus::
                    MinimumInputPitchNotBlockConstant;
            }
            if (maximumInputPitch.connected
                && maximumValue != firstMaximum)
            {
                return MonotonizerControlStatus::
                    MaximumInputPitchNotBlockConstant;
            }
        }
        return MonotonizerControlStatus::Ok;
    };

    const auto firstFrequency = selected(frequency, 0);
    const auto firstMinimum = selected(minimumInputPitch, 0);
    const auto firstMaximum = selected(maximumInputPitch, 0);
    const auto firstStatus = validateFrame(
        firstFrequency,
        firstMinimum,
        firstMaximum,
        firstFrequency,
        firstMinimum,
        firstMaximum,
        false);
    if (firstStatus != MonotonizerControlStatus::Ok)
        return firstStatus;
    for (std::size_t frame = 1; frame < frameCount; ++frame)
    {
        const auto status = validateFrame(
            selected(frequency, frame),
            selected(minimumInputPitch, frame),
            selected(maximumInputPitch, frame),
            firstFrequency,
            firstMinimum,
            firstMaximum,
            true);
        if (status != MonotonizerControlStatus::Ok)
            return status;
    }

    const auto ratio = phaseVocoderKnownPitchRatio(
        firstFrequency, firstMinimum, firstMaximum);
    if (!activeProfile && ratio != 1.0)
        return MonotonizerControlStatus::ProfileMismatch;
    const auto interval = phaseVocoderFrequencyScaleInterval(ratio);
    if (!isSupportedPhaseVocoderPitchInterval(interval))
        return MonotonizerControlStatus::InvalidInterval;
    decision.activeProfile = activeProfile;
    decision.frequency = firstFrequency;
    decision.sourcePitch = firstMinimum;
    decision.ratio = ratio;
    decision.intervalSemitones = interval;
    return MonotonizerControlStatus::Ok;
}

PreparedPhaseVocoderPitchShifter::PreparedPhaseVocoderPitchShifter(
    double sampleRate,
    std::size_t maximumBlockFrames,
    double intervalSemitones,
    PhaseVocoderSpectralEnvelopePolicy envelopePolicy,
    PhaseVocoderPitchProfile pitchProfile)
    : sampleRate_(validatedSampleRate(sampleRate)),
      maximumBlockFrames_(maximumBlockFrames),
      initialIntervalSemitones_(validatedInterval(intervalSemitones, pitchProfile)),
      targetIntervalSemitones_(initialIntervalSemitones_),
      currentRatio_(phaseVocoderPitchRatio(initialIntervalSemitones_, pitchProfile)),
      targetRatio_(currentRatio_),
      envelopePolicy_(envelopePolicy),
      pitchProfile_(pitchProfile),
      forward_(pitchConfiguration),
      inverse_(
          pitchConfiguration,
          validatedFrameCapacity(maximumBlockFrames),
          InverseStftSynthesisWindowPolicy::ApplyAnalysisWindow),
      analysisFrames_(
          validatedFrameCapacity(maximumBlockFrames),
          phaseVocoderPitchFrameSize / 2 + 1),
      synthesisFrames_(
          validatedFrameCapacity(maximumBlockFrames),
          phaseVocoderPitchFrameSize / 2 + 1),
      previousAnalysisPhases_(phaseVocoderPitchFrameSize / 2 + 1),
      synthesisPhases_(phaseVocoderPitchFrameSize / 2 + 1),
      analysisMagnitudes_(phaseVocoderPitchFrameSize / 2 + 1),
      spectralLogMagnitudes_(phaseVocoderPitchFrameSize / 2 + 1),
      spectralLogEnvelope_(phaseVocoderPitchFrameSize / 2 + 1),
      spectralResidualMagnitudes_(phaseVocoderPitchFrameSize / 2 + 1),
      previousAnalysisMagnitudes_(phaseVocoderPitchFrameSize / 2 + 1),
      analysisAdvances_(phaseVocoderPitchFrameSize / 2 + 1),
      mappedMagnitudes_(phaseVocoderPitchFrameSize / 2 + 1),
      mappedAnalysisPhases_(phaseVocoderPitchFrameSize / 2 + 1),
      mappedAdvances_(phaseVocoderPitchFrameSize / 2 + 1),
      identityPeakBins_(phaseVocoderPitchFrameSize / 2 + 1),
      identityPeakOwners_(phaseVocoderPitchFrameSize / 2 + 1),
      identityPeakMatches_(phaseVocoderPitchFrameSize / 2 + 1),
      previousIdentityPeakBins_(phaseVocoderPitchFrameSize / 2 + 1),
      previousIdentityPeakPhases_(phaseVocoderPitchFrameSize / 2 + 1),
      synthesisBins_(phaseVocoderPitchFrameSize / 2 + 1)
{
    if (maximumBlockFrames_ == 0 || !std::isfinite(currentRatio_)
        || currentRatio_ <= 0.0
        || (envelopePolicy_
                != PhaseVocoderSpectralEnvelopePolicy::ScaleWithSpectrum
            && envelopePolicy_
                != PhaseVocoderSpectralEnvelopePolicy::PreserveAbsoluteFrequency))
    {
        throw std::invalid_argument("Invalid phase-vocoder pitch profile");
    }
}

PhaseVocoderPitchStatus PreparedPhaseVocoderPitchShifter::process(
    std::span<const double> input,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> output) noexcept
{
    return processWithInterval(
        input,
        initialIntervalSemitones_,
        blockStart,
        compiledInputLatencyFrames,
        output);
}

PhaseVocoderPitchStatus
PreparedPhaseVocoderPitchShifter::processWithInterval(
    std::span<const double> input,
    double intervalSemitones,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> output) noexcept
{
    if (!std::isfinite(sampleRate_) || sampleRate_ <= 0.0)
        return PhaseVocoderPitchStatus::InvalidSampleRate;
    if (!isSupportedPhaseVocoderPitchInterval(intervalSemitones, pitchProfile_))
    {
        return PhaseVocoderPitchStatus::InvalidInterval;
    }
    if (input.size() != output.size())
        return PhaseVocoderPitchStatus::InvalidBufferShape;
    if (input.size() > maximumBlockFrames_)
        return PhaseVocoderPitchStatus::BlockCapacityExceeded;
    if (compiledInputLatencyFrames
        > std::numeric_limits<std::size_t>::max()
            - phaseVocoderPitchLatencyFrames)
    {
        return PhaseVocoderPitchStatus::TimestampOverflow;
    }
    if (input.empty())
        return PhaseVocoderPitchStatus::Ok;

    analysisFrames_.reset();
    synthesisFrames_.reset();
    const auto forwardStatus = forward_.process(
        input,
        blockStart,
        analysisFrames_,
        compiledInputLatencyFrames);
    if (forwardStatus != NativeStftStatus::Ok)
    {
        if (forwardStatus == NativeStftStatus::DiscontinuousBlock)
            previousIdentityPeakCount_ = 0;
        return translateStatus(forwardStatus);
    }

    const auto requestedRatio = phaseVocoderPitchRatio(intervalSemitones, pitchProfile_);
    if (!std::isfinite(requestedRatio) || requestedRatio <= 0.0)
        return PhaseVocoderPitchStatus::NumericOverflow;
    const auto transformStatus = transformFrames(requestedRatio);
    if (transformStatus != PhaseVocoderPitchStatus::Ok)
        return transformStatus;
    targetIntervalSemitones_ = intervalSemitones;

    const auto inverseStatus = inverse_.process(
        synthesisFrames_.frames(),
        blockStart,
        compiledInputLatencyFrames + phaseVocoderPitchLatencyFrames,
        output);
    return translateStatus(inverseStatus);
}

PhaseVocoderPitchStatus
PreparedPhaseVocoderPitchShifter::transformFrames(
    double requestedRatio) noexcept
{
    constexpr auto binCount = phaseVocoderPitchFrameSize / 2 + 1;
    constexpr auto expectedScale =
        2.0 * std::numbers::pi
        * static_cast<double>(phaseVocoderPitchHopSize)
        / static_cast<double>(phaseVocoderPitchFrameSize);

    if (requestedRatio != targetRatio_)
    {
        targetRatio_ = requestedRatio;
        ratioRampFramesRemaining_ = phaseVocoderPitchRatioRampHops;
    }

    for (const auto& frame : analysisFrames_.frames())
    {
        if (frame.bins.size() != binCount)
            return PhaseVocoderPitchStatus::InternalSpectralFailure;

        double positiveSpectralFlux = 0.0;
        double currentSpectralMagnitude = 0.0;
        for (std::size_t bin = 0; bin < binCount; ++bin)
        {
            const auto magnitude = std::abs(frame.bins[bin]);
            const auto phase = std::arg(frame.bins[bin]);
            const auto expected = expectedScale * static_cast<double>(bin);
            const auto advance = phaseInitialized_
                ? expected + wrappedPhase(
                    phase - previousAnalysisPhases_[bin] - expected)
                : expected;
            if (!std::isfinite(magnitude) || !std::isfinite(phase)
                || !std::isfinite(advance))
            {
                return PhaseVocoderPitchStatus::NumericOverflow;
            }
            analysisMagnitudes_[bin] = magnitude;
            analysisAdvances_[bin] = advance;
            previousAnalysisPhases_[bin] = phase;
            currentSpectralMagnitude += magnitude;
            if (magnitudeHistoryInitialized_)
            {
                positiveSpectralFlux += std::max(
                    magnitude - previousAnalysisMagnitudes_[bin],
                    0.0);
            }
        }
        if (!std::isfinite(positiveSpectralFlux)
            || !std::isfinite(currentSpectralMagnitude))
        {
            return PhaseVocoderPitchStatus::NumericOverflow;
        }

        if (envelopePolicy_
            == PhaseVocoderSpectralEnvelopePolicy::PreserveAbsoluteFrequency)
        {
            for (std::size_t bin = 0; bin < binCount; ++bin)
            {
                spectralLogMagnitudes_[bin] = std::log(std::max(
                    analysisMagnitudes_[bin],
                    phaseVocoderIdentityPeakEnergyFloor));
            }
            auto lower = std::size_t{0};
            auto upper = std::min(
                phaseVocoderSpectralEnvelopeRadiusBins,
                binCount - 1);
            double logMagnitudeSum = 0.0;
            for (std::size_t bin = lower; bin <= upper; ++bin)
                logMagnitudeSum += spectralLogMagnitudes_[bin];
            for (std::size_t bin = 0; bin < binCount; ++bin)
            {
                const auto count = upper - lower + 1;
                const auto logEnvelope = logMagnitudeSum
                    / static_cast<double>(count);
                const auto envelope = std::exp(logEnvelope);
                const auto residual = analysisMagnitudes_[bin] / envelope;
                if (!std::isfinite(logEnvelope)
                    || !std::isfinite(envelope) || envelope <= 0.0
                    || !std::isfinite(residual) || residual < 0.0)
                {
                    return PhaseVocoderPitchStatus::NumericOverflow;
                }
                spectralLogEnvelope_[bin] = logEnvelope;
                spectralResidualMagnitudes_[bin] = residual;

                if (bin + 1 == binCount)
                    continue;
                const auto nextLower = bin + 1
                        > phaseVocoderSpectralEnvelopeRadiusBins
                    ? bin + 1 - phaseVocoderSpectralEnvelopeRadiusBins
                    : 0;
                const auto nextUpper = std::min(
                    bin + 1 + phaseVocoderSpectralEnvelopeRadiusBins,
                    binCount - 1);
                while (lower < nextLower)
                    logMagnitudeSum -= spectralLogMagnitudes_[lower++];
                while (upper < nextUpper)
                    logMagnitudeSum += spectralLogMagnitudes_[++upper];
            }
        }

        const auto refractory = transientRefractoryHopsRemaining_ != 0;
        if (transientRefractoryHopsRemaining_ != 0)
            --transientRefractoryHopsRemaining_;
        const auto normalizedFlux = currentSpectralMagnitude
                >= phaseVocoderTransientEnergyFloor
            ? positiveSpectralFlux / currentSpectralMagnitude
            : 0.0;
        if (!std::isfinite(normalizedFlux))
            return PhaseVocoderPitchStatus::NumericOverflow;
        const auto detectedTransient = magnitudeHistoryInitialized_
            && !refractory
            && normalizedFlux >= phaseVocoderTransientFluxThreshold;
        const auto transientAnchor = !phaseInitialized_
            || detectedTransient;

        if (ratioRampFramesRemaining_ != 0)
        {
            currentRatio_ += (targetRatio_ - currentRatio_)
                / static_cast<double>(ratioRampFramesRemaining_);
            --ratioRampFramesRemaining_;
        }
        else
        {
            currentRatio_ = targetRatio_;
        }
        if (!std::isfinite(currentRatio_) || currentRatio_ <= 0.0)
            return PhaseVocoderPitchStatus::NumericOverflow;

        const auto translatePeakRegions =
            pitchProfile_ == PhaseVocoderPitchProfile::TimeFrequencyCorrection
            && currentRatio_ > maximumPhaseVocoderFrequencyScale;
        std::size_t sourcePeakCount = 0;
        std::size_t preservedDcRegionEnd = 0;
        bool preserveDcRegion = false;
        if (translatePeakRegions)
        {
            // Dilating a Hann lobe by more than an octave makes it too narrow
            // in time for this fixed synthesis hop, producing hop-rate sidebands.
            // Translate each peak's region without widening its lobe. Phase
            // advances still carry the continuous (non-bin-rounded) frequency.
            // A quiet partial must retain its own translated region even
            // when a stronger tone or DC offset dominates the frame.
            constexpr auto peakFloor = phaseVocoderIdentityPeakEnergyFloor;
            if (analysisMagnitudes_[0] >= phaseVocoderIdentityPeakEnergyFloor
                && analysisMagnitudes_[0] >= analysisMagnitudes_[1])
                identityPeakBins_[sourcePeakCount++] = 0;
            for (std::size_t bin = 1; bin + 1 < binCount; ++bin)
                if (analysisMagnitudes_[bin] >= peakFloor
                    && analysisMagnitudes_[bin] > analysisMagnitudes_[bin - 1]
                    && analysisMagnitudes_[bin] >= analysisMagnitudes_[bin + 1])
                    identityPeakBins_[sourcePeakCount++] = bin;
            preserveDcRegion = sourcePeakCount != 0 && identityPeakBins_[0] == 0;
            if (preserveDcRegion)
                preservedDcRegionEnd = 1; // Periodic Hann's constant-signal support.
        }
        std::size_t sourcePeakIndex = 0;
        for (std::size_t outputBin = 0;
             outputBin < binCount;
             ++outputBin)
        {
            auto sourcePosition =
                static_cast<double>(outputBin) / currentRatio_;
            double sourceRegionLower = 0;
            double sourceRegionUpper = static_cast<double>(binCount - 1);
            if (translatePeakRegions && sourcePeakCount != 0)
            {
                while (sourcePeakIndex + 1 < sourcePeakCount
                    && static_cast<double>(outputBin) > currentRatio_ * 0.5
                        * static_cast<double>(identityPeakBins_[sourcePeakIndex]
                            + identityPeakBins_[sourcePeakIndex + 1]))
                    ++sourcePeakIndex;
                const auto peak = identityPeakBins_[sourcePeakIndex];
                const auto shift = peak == 0 ? 0.0 : std::round((currentRatio_ - 1)
                    * analysisAdvances_[peak] / expectedScale);
                sourcePosition = static_cast<double>(outputBin) - shift;
                if (sourcePeakIndex != 0)
                    sourceRegionLower = std::floor(0.5 * static_cast<double>(
                        identityPeakBins_[sourcePeakIndex - 1] + peak)) + 1;
                if (sourcePeakIndex + 1 < sourcePeakCount)
                    sourceRegionUpper = std::floor(0.5 * static_cast<double>(
                        peak + identityPeakBins_[sourcePeakIndex + 1]));
            }
            if (sourcePosition < sourceRegionLower || sourcePosition > sourceRegionUpper)
            {
                mappedMagnitudes_[outputBin] = 0.0;
                mappedAnalysisPhases_[outputBin] = 0.0;
                mappedAdvances_[outputBin] = 0.0;
                continue;
            }
            const auto first = static_cast<std::size_t>(sourcePosition);
            const auto second = std::min(first + 1, binCount - 1);
            const auto fraction = sourcePosition
                - static_cast<double>(first);
            if (envelopePolicy_
                == PhaseVocoderSpectralEnvelopePolicy::PreserveAbsoluteFrequency)
            {
                const auto residual = spectralResidualMagnitudes_[first]
                    + fraction
                        * (spectralResidualMagnitudes_[second]
                           - spectralResidualMagnitudes_[first]);
                const auto destinationEnvelope = std::exp(
                    spectralLogEnvelope_[outputBin]);
                mappedMagnitudes_[outputBin] = residual
                    * destinationEnvelope;
            }
            else
            {
                mappedMagnitudes_[outputBin] = analysisMagnitudes_[first]
                    + fraction
                        * (analysisMagnitudes_[second]
                           - analysisMagnitudes_[first]);
            }
            mappedAnalysisPhases_[outputBin] = interpolatePhase(
                previousAnalysisPhases_[first],
                previousAnalysisPhases_[second],
                fraction);
            mappedAdvances_[outputBin] = currentRatio_
                * (analysisAdvances_[first]
                   + fraction
                       * (analysisAdvances_[second]
                          - analysisAdvances_[first]));
            if (!std::isfinite(mappedMagnitudes_[outputBin])
                || !std::isfinite(mappedAnalysisPhases_[outputBin])
                || !std::isfinite(mappedAdvances_[outputBin]))
            {
                return PhaseVocoderPitchStatus::NumericOverflow;
            }
        }

        std::size_t acceptedPeakIdentityCount = 0;
        std::size_t matchedPeakIdentityCount = 0;
        if (transientAnchor)
        {
            std::ranges::copy(
                mappedAnalysisPhases_, synthesisPhases_.begin());
        }
        else
        {
            const auto strongest = std::ranges::max_element(
                mappedMagnitudes_);
            const auto strongestMagnitude = *strongest;
            const auto peakFloor = translatePeakRegions
                ? phaseVocoderIdentityPeakEnergyFloor
                : std::max(phaseVocoderIdentityPeakEnergyFloor,
                    strongestMagnitude * phaseVocoderIdentityPeakRelativeFloor);
            std::size_t peakCount = 0;
            for (std::size_t bin = 1; bin + 1 < binCount; ++bin)
            {
                const auto magnitude = mappedMagnitudes_[bin];
                if (magnitude >= peakFloor
                    && magnitude > mappedMagnitudes_[bin - 1]
                    && magnitude >= mappedMagnitudes_[bin + 1])
                {
                    identityPeakBins_[peakCount++] = bin;
                }
            }

            if (peakCount == 0 || (peakCount < 2
                && (pitchProfile_ != PhaseVocoderPitchProfile::TimeFrequencyCorrection
                    || currentRatio_ <= maximumPhaseVocoderFrequencyScale)))
            {
                for (std::size_t bin = 0; bin < binCount; ++bin)
                {
                    synthesisPhases_[bin] = wrappedPhase(
                        synthesisPhases_[bin] + mappedAdvances_[bin]);
                }
            }
            else
            {
                std::ranges::fill_n(
                    identityPeakMatches_.begin(),
                    peakCount,
                    binCount);
                std::size_t previousSearchStart = 0;
                for (std::size_t peak = 0; peak < peakCount; ++peak)
                {
                    const auto currentBin = identityPeakBins_[peak];
                    while (previousSearchStart
                               < previousIdentityPeakCount_
                           && previousIdentityPeakBins_[previousSearchStart]
                                   + phaseVocoderPeakIdentityMaximumDisplacementBins
                               < currentBin)
                    {
                        ++previousSearchStart;
                    }

                    auto bestPrevious = binCount;
                    auto bestDistance =
                        phaseVocoderPeakIdentityMaximumDisplacementBins + 1;
                    for (auto previous = previousSearchStart;
                         previous < previousIdentityPeakCount_;
                         ++previous)
                    {
                        const auto previousBin =
                            previousIdentityPeakBins_[previous];
                        if (previousBin
                            > currentBin
                                + phaseVocoderPeakIdentityMaximumDisplacementBins)
                        {
                            break;
                        }
                        const auto distance = previousBin <= currentBin
                            ? currentBin - previousBin
                            : previousBin - currentBin;
                        if (distance < bestDistance)
                        {
                            bestDistance = distance;
                            bestPrevious = previous;
                        }
                    }
                    if (bestPrevious != binCount)
                    {
                        identityPeakMatches_[peak] = bestPrevious;
                        previousSearchStart = bestPrevious + 1;
                        ++matchedPeakIdentityCount;
                    }
                }

                std::size_t peakIndex = 0;
                for (std::size_t bin = 0; bin < binCount; ++bin)
                {
                    while (peakIndex + 1 < peakCount)
                    {
                        const auto lower = identityPeakBins_[peakIndex];
                        const auto upper = identityPeakBins_[peakIndex + 1];
                        const auto midpoint = lower + (upper - lower) / 2;
                        if (bin <= midpoint)
                            break;
                        ++peakIndex;
                    }
                    identityPeakOwners_[bin] =
                        identityPeakBins_[peakIndex];
                }

                for (std::size_t peak = 0; peak < peakCount; ++peak)
                {
                    const auto bin = identityPeakBins_[peak];
                    const auto previous = identityPeakMatches_[peak];
                    synthesisPhases_[bin] = previous < previousIdentityPeakCount_
                        ? wrappedPhase(
                            previousIdentityPeakPhases_[previous]
                            + mappedAdvances_[bin])
                        : mappedAnalysisPhases_[bin];
                    if (!std::isfinite(synthesisPhases_[bin]))
                    {
                        return PhaseVocoderPitchStatus::NumericOverflow;
                    }
                }
                for (std::size_t bin = 0; bin < binCount; ++bin)
                {
                    const auto owner = identityPeakOwners_[bin];
                    if (owner >= binCount)
                        return PhaseVocoderPitchStatus::InternalSpectralFailure;
                    if (owner != bin)
                    {
                        synthesisPhases_[bin] = wrappedPhase(
                            synthesisPhases_[owner]
                            + wrappedPhase(
                                mappedAnalysisPhases_[bin]
                                - mappedAnalysisPhases_[owner]));
                    }
                }
                if (identityLockedFrameCount_
                    != std::numeric_limits<std::size_t>::max())
                {
                    ++identityLockedFrameCount_;
                }
                acceptedPeakIdentityCount = peakCount;
            }
        }

        for (std::size_t outputBin = 0;
             outputBin < binCount;
             ++outputBin)
        {
            // Keep DC's complete window lobe at zero frequency, independent
            // of the nearest nonzero peak's phase propagation.
            synthesisBins_[outputBin] = preserveDcRegion && outputBin <= preservedDcRegionEnd
                ? frame.bins[outputBin]
                : std::polar(mappedMagnitudes_[outputBin], synthesisPhases_[outputBin]);
        }
        synthesisBins_.front().imag(0.0);
        synthesisBins_.back().imag(0.0);

        const auto appendStatus = synthesisFrames_.append(
            frame.analysisTimestamp,
            frame.sourceSequence,
            frame.frameSize,
            frame.hopSize,
            frame.window,
            frame.channelIndex,
            synthesisBins_);
        if (appendStatus != SpectralFrameStatus::Ok)
            return PhaseVocoderPitchStatus::InternalSpectralFailure;
        previousIdentityPeakCount_ = acceptedPeakIdentityCount;
        for (std::size_t peak = 0;
             peak < acceptedPeakIdentityCount;
             ++peak)
        {
            const auto bin = identityPeakBins_[peak];
            previousIdentityPeakBins_[peak] = bin;
            previousIdentityPeakPhases_[peak] = synthesisPhases_[bin];
        }
        if (peakIdentityMatchCount_
            > std::numeric_limits<std::size_t>::max()
                - matchedPeakIdentityCount)
        {
            peakIdentityMatchCount_ =
                std::numeric_limits<std::size_t>::max();
        }
        else
        {
            peakIdentityMatchCount_ += matchedPeakIdentityCount;
        }
        std::ranges::copy(
            analysisMagnitudes_, previousAnalysisMagnitudes_.begin());
        magnitudeHistoryInitialized_ = true;
        if (transientAnchor)
        {
            if (detectedTransient
                || currentSpectralMagnitude
                    >= phaseVocoderTransientEnergyFloor)
            {
                transientRefractoryHopsRemaining_ =
                    phaseVocoderTransientRefractoryHops;
            }
            if (transientPhaseAnchorCount_
                != std::numeric_limits<std::size_t>::max())
            {
                ++transientPhaseAnchorCount_;
            }
        }
        phaseInitialized_ = true;
    }
    return PhaseVocoderPitchStatus::Ok;
}

std::unique_ptr<PreparedPhaseVocoderPitchShifter>
PreparedPhaseVocoderPitchShifter::cloneProcessingState() const
{
    auto result = std::make_unique<PreparedPhaseVocoderPitchShifter>(
        sampleRate_, maximumBlockFrames_, initialIntervalSemitones_, envelopePolicy_, pitchProfile_);
    if (!result->copyProcessingStateFrom(*this))
        throw std::logic_error("Phase-vocoder processing snapshot has incompatible preparation");
    return result;
}

bool PreparedPhaseVocoderPitchShifter::copyProcessingStateFrom(
    const PreparedPhaseVocoderPitchShifter& source) noexcept
{
    const auto sameSize = [](const auto& left, const auto& right) {
        return left.size() == right.size();
    };
    if (sampleRate_ != source.sampleRate_
        || maximumBlockFrames_ != source.maximumBlockFrames_
        || initialIntervalSemitones_ != source.initialIntervalSemitones_
        || envelopePolicy_ != source.envelopePolicy_
        || pitchProfile_ != source.pitchProfile_
        || !sameSize(previousAnalysisPhases_, source.previousAnalysisPhases_)
        || !sameSize(synthesisPhases_, source.synthesisPhases_)
        || !sameSize(analysisMagnitudes_, source.analysisMagnitudes_)
        || !sameSize(spectralLogMagnitudes_, source.spectralLogMagnitudes_)
        || !sameSize(spectralLogEnvelope_, source.spectralLogEnvelope_)
        || !sameSize(
            spectralResidualMagnitudes_, source.spectralResidualMagnitudes_)
        || !sameSize(
            previousAnalysisMagnitudes_, source.previousAnalysisMagnitudes_)
        || !sameSize(analysisAdvances_, source.analysisAdvances_)
        || !sameSize(mappedMagnitudes_, source.mappedMagnitudes_)
        || !sameSize(mappedAnalysisPhases_, source.mappedAnalysisPhases_)
        || !sameSize(mappedAdvances_, source.mappedAdvances_)
        || !sameSize(identityPeakBins_, source.identityPeakBins_)
        || !sameSize(identityPeakOwners_, source.identityPeakOwners_)
        || !sameSize(identityPeakMatches_, source.identityPeakMatches_)
        || !sameSize(
            previousIdentityPeakBins_, source.previousIdentityPeakBins_)
        || !sameSize(
            previousIdentityPeakPhases_, source.previousIdentityPeakPhases_)
        || !sameSize(synthesisBins_, source.synthesisBins_)
        || !forward_.copyProcessingStateFrom(source.forward_)
        || !inverse_.copyProcessingStateFrom(source.inverse_))
    {
        return false;
    }
    std::ranges::copy(
        source.previousAnalysisPhases_, previousAnalysisPhases_.begin());
    std::ranges::copy(source.synthesisPhases_, synthesisPhases_.begin());
    std::ranges::copy(source.analysisMagnitudes_, analysisMagnitudes_.begin());
    std::ranges::copy(
        source.spectralLogMagnitudes_, spectralLogMagnitudes_.begin());
    std::ranges::copy(
        source.spectralLogEnvelope_, spectralLogEnvelope_.begin());
    std::ranges::copy(
        source.spectralResidualMagnitudes_,
        spectralResidualMagnitudes_.begin());
    std::ranges::copy(
        source.previousAnalysisMagnitudes_,
        previousAnalysisMagnitudes_.begin());
    std::ranges::copy(source.analysisAdvances_, analysisAdvances_.begin());
    std::ranges::copy(source.mappedMagnitudes_, mappedMagnitudes_.begin());
    std::ranges::copy(
        source.mappedAnalysisPhases_, mappedAnalysisPhases_.begin());
    std::ranges::copy(source.mappedAdvances_, mappedAdvances_.begin());
    std::ranges::copy(source.identityPeakBins_, identityPeakBins_.begin());
    std::ranges::copy(source.identityPeakOwners_, identityPeakOwners_.begin());
    std::ranges::copy(
        source.identityPeakMatches_, identityPeakMatches_.begin());
    std::ranges::copy(
        source.previousIdentityPeakBins_, previousIdentityPeakBins_.begin());
    std::ranges::copy(
        source.previousIdentityPeakPhases_,
        previousIdentityPeakPhases_.begin());
    std::ranges::copy(source.synthesisBins_, synthesisBins_.begin());
    targetIntervalSemitones_ = source.targetIntervalSemitones_;
    currentRatio_ = source.currentRatio_;
    targetRatio_ = source.targetRatio_;
    ratioRampFramesRemaining_ = source.ratioRampFramesRemaining_;
    phaseInitialized_ = source.phaseInitialized_;
    magnitudeHistoryInitialized_ = source.magnitudeHistoryInitialized_;
    transientRefractoryHopsRemaining_ =
        source.transientRefractoryHopsRemaining_;
    transientPhaseAnchorCount_ = source.transientPhaseAnchorCount_;
    identityLockedFrameCount_ = source.identityLockedFrameCount_;
    peakIdentityMatchCount_ = source.peakIdentityMatchCount_;
    previousIdentityPeakCount_ = source.previousIdentityPeakCount_;
    return true;
}

bool PreparedPhaseVocoderPitchShifter::retimeNextBlockAbsoluteFrame(
    std::uint64_t absoluteFrame) noexcept
{
    return forward_.retimeNextBlockAbsoluteFrame(absoluteFrame)
        && inverse_.retimeNextBlockAbsoluteFrame(absoluteFrame);
}

void PreparedPhaseVocoderPitchShifter::reset() noexcept
{
    forward_.reset();
    inverse_.reset();
    analysisFrames_.reset();
    synthesisFrames_.reset();
    std::ranges::fill(previousAnalysisPhases_, 0.0);
    std::ranges::fill(synthesisPhases_, 0.0);
    std::ranges::fill(analysisMagnitudes_, 0.0);
    std::ranges::fill(spectralLogMagnitudes_, 0.0);
    std::ranges::fill(spectralLogEnvelope_, 0.0);
    std::ranges::fill(spectralResidualMagnitudes_, 0.0);
    std::ranges::fill(previousAnalysisMagnitudes_, 0.0);
    std::ranges::fill(analysisAdvances_, 0.0);
    std::ranges::fill(mappedMagnitudes_, 0.0);
    std::ranges::fill(mappedAnalysisPhases_, 0.0);
    std::ranges::fill(mappedAdvances_, 0.0);
    std::ranges::fill(identityPeakBins_, 0);
    std::ranges::fill(identityPeakOwners_, 0);
    std::ranges::fill(identityPeakMatches_, 0);
    std::ranges::fill(previousIdentityPeakBins_, 0);
    std::ranges::fill(previousIdentityPeakPhases_, 0.0);
    std::ranges::fill(synthesisBins_, std::complex<double>{});
    targetIntervalSemitones_ = initialIntervalSemitones_;
    currentRatio_ = phaseVocoderPitchRatio(initialIntervalSemitones_, pitchProfile_);
    targetRatio_ = currentRatio_;
    ratioRampFramesRemaining_ = 0;
    phaseInitialized_ = false;
    magnitudeHistoryInitialized_ = false;
    transientRefractoryHopsRemaining_ = 0;
    transientPhaseAnchorCount_ = 0;
    identityLockedFrameCount_ = 0;
    peakIdentityMatchCount_ = 0;
    previousIdentityPeakCount_ = 0;
}

double PreparedPhaseVocoderPitchShifter::sampleRate() const noexcept
{
    return sampleRate_;
}

std::size_t
PreparedPhaseVocoderPitchShifter::maximumBlockFrames() const noexcept
{
    return maximumBlockFrames_;
}

double
PreparedPhaseVocoderPitchShifter::intervalSemitones() const noexcept
{
    return targetIntervalSemitones_;
}

double PreparedPhaseVocoderPitchShifter::ratio() const noexcept
{
    return targetRatio_;
}

PhaseVocoderPitchProfile PreparedPhaseVocoderPitchShifter::pitchProfile() const noexcept
{
    return pitchProfile_;
}

PhaseVocoderSpectralEnvelopePolicy PreparedPhaseVocoderPitchShifter::
spectralEnvelopePolicy() const noexcept
{
    return envelopePolicy_;
}

std::size_t PreparedPhaseVocoderPitchShifter::
transientPhaseAnchorCount() const noexcept
{
    return transientPhaseAnchorCount_;
}

std::size_t PreparedPhaseVocoderPitchShifter::
identityLockedFrameCount() const noexcept
{
    return identityLockedFrameCount_;
}

std::size_t PreparedPhaseVocoderPitchShifter::
peakIdentityMatchCount() const noexcept
{
    return peakIdentityMatchCount_;
}

std::size_t PreparedPhaseVocoderPitchShifter::
activePeakIdentityCount() const noexcept
{
    return previousIdentityPeakCount_;
}

PreparedDynamicPhaseVocoderPitchShifter::
PreparedDynamicPhaseVocoderPitchShifter(
    double sampleRate,
    std::size_t maximumBlockFrames,
    double initialIntervalSemitones,
    PhaseVocoderSpectralEnvelopePolicy envelopePolicy,
    PhaseVocoderPitchProfile pitchProfile)
    : initialIntervalSemitones_(
          validatedInterval(initialIntervalSemitones, pitchProfile)),
      maximumBlockFrames_(maximumBlockFrames),
      spectral_(
          sampleRate,
          maximumBlockFrames,
          initialIntervalSemitones,
          envelopePolicy,
          pitchProfile),
      spectralScratch_(maximumBlockFrames),
      delayScratch_(maximumBlockFrames),
      delayLine_(phaseVocoderPitchLatencyFrames),
      spectralMix_(initialIntervalSemitones_ == 0.0 ? 0.0 : 1.0),
      targetSpectralMix_(spectralMix_)
{
    if (maximumBlockFrames_ == 0)
        throw std::invalid_argument("Invalid dynamic pitch block capacity");
}

PhaseVocoderPitchStatus PreparedDynamicPhaseVocoderPitchShifter::process(
    std::span<const double> input,
    double intervalSemitones,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> output) noexcept
{
    if (!isSupportedPhaseVocoderPitchInterval(intervalSemitones, spectral_.pitchProfile()))
        return PhaseVocoderPitchStatus::InvalidInterval;
    if (input.size() != output.size())
        return PhaseVocoderPitchStatus::InvalidBufferShape;
    if (input.size() > maximumBlockFrames_)
        return PhaseVocoderPitchStatus::BlockCapacityExceeded;
    if (std::ranges::any_of(input, [](double sample) {
            return !std::isfinite(sample);
        }))
    {
        return PhaseVocoderPitchStatus::NonFiniteInput;
    }

    const auto spectralOutput = std::span<double>{spectralScratch_}.first(
        input.size());
    const auto delayedOutput = std::span<double>{delayScratch_}.first(
        input.size());
    const auto spectralStatus = spectral_.processWithInterval(
        input,
        intervalSemitones,
        blockStart,
        compiledInputLatencyFrames,
        spectralOutput);
    if (spectralStatus != PhaseVocoderPitchStatus::Ok)
        return spectralStatus;
    if (input.empty())
        return PhaseVocoderPitchStatus::Ok;

    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        delayedOutput[frame] = delayLine_[delayWriteIndex_];
        delayLine_[delayWriteIndex_] = input[frame];
        delayWriteIndex_ = (delayWriteIndex_ + 1) % delayLine_.size();
    }

    const auto requestedMix = intervalSemitones == 0.0 ? 0.0 : 1.0;
    if (requestedMix != targetSpectralMix_)
    {
        targetSpectralMix_ = requestedMix;
        transitionFramesRemaining_ =
            dynamicPhaseVocoderPitchTransitionFrames;
        spectralMixStep_ = (targetSpectralMix_ - spectralMix_)
            / static_cast<double>(transitionFramesRemaining_);
    }

    constexpr auto halfPi = std::numbers::pi * 0.5;
    for (std::size_t frame = 0; frame < input.size(); ++frame)
    {
        const auto delayedGain = std::cos(halfPi * spectralMix_);
        const auto spectralGain = std::sin(halfPi * spectralMix_);
        const auto mixed = delayedOutput[frame] * delayedGain
            + spectralOutput[frame] * spectralGain;
        if (!std::isfinite(mixed))
            return PhaseVocoderPitchStatus::NumericOverflow;
        delayedOutput[frame] = mixed;

        if (transitionFramesRemaining_ != 0)
        {
            spectralMix_ += spectralMixStep_;
            --transitionFramesRemaining_;
            if (transitionFramesRemaining_ == 0)
                spectralMix_ = targetSpectralMix_;
        }
    }
    std::ranges::copy(delayedOutput, output.begin());
    return PhaseVocoderPitchStatus::Ok;
}

void PreparedDynamicPhaseVocoderPitchShifter::reset() noexcept
{
    spectral_.reset();
    std::ranges::fill(spectralScratch_, 0.0);
    std::ranges::fill(delayScratch_, 0.0);
    std::ranges::fill(delayLine_, 0.0);
    delayWriteIndex_ = 0;
    spectralMix_ = initialIntervalSemitones_ == 0.0 ? 0.0 : 1.0;
    targetSpectralMix_ = spectralMix_;
    spectralMixStep_ = 0.0;
    transitionFramesRemaining_ = 0;
}

std::unique_ptr<PreparedDynamicPhaseVocoderPitchShifter>
PreparedDynamicPhaseVocoderPitchShifter::cloneProcessingState() const
{
    auto result = std::make_unique<PreparedDynamicPhaseVocoderPitchShifter>(
        spectral_.sampleRate(), maximumBlockFrames_, initialIntervalSemitones_,
        spectral_.spectralEnvelopePolicy(), spectral_.pitchProfile());
    if (!result->copyProcessingStateFrom(*this))
        throw std::logic_error("Dynamic phase-vocoder processing snapshot has incompatible preparation");
    return result;
}

bool PreparedDynamicPhaseVocoderPitchShifter::copyProcessingStateFrom(
    const PreparedDynamicPhaseVocoderPitchShifter& source) noexcept
{
    if (initialIntervalSemitones_ != source.initialIntervalSemitones_
        || maximumBlockFrames_ != source.maximumBlockFrames_
        || spectralScratch_.size() != source.spectralScratch_.size()
        || delayScratch_.size() != source.delayScratch_.size()
        || delayLine_.size() != source.delayLine_.size()
        || !spectral_.copyProcessingStateFrom(source.spectral_))
    {
        return false;
    }
    std::ranges::copy(source.spectralScratch_, spectralScratch_.begin());
    std::ranges::copy(source.delayScratch_, delayScratch_.begin());
    std::ranges::copy(source.delayLine_, delayLine_.begin());
    delayWriteIndex_ = source.delayWriteIndex_;
    spectralMix_ = source.spectralMix_;
    targetSpectralMix_ = source.targetSpectralMix_;
    spectralMixStep_ = source.spectralMixStep_;
    transitionFramesRemaining_ = source.transitionFramesRemaining_;
    return true;
}

bool PreparedDynamicPhaseVocoderPitchShifter::
retimeNextBlockAbsoluteFrame(std::uint64_t absoluteFrame) noexcept
{
    return spectral_.retimeNextBlockAbsoluteFrame(absoluteFrame);
}

double PreparedDynamicPhaseVocoderPitchShifter::
initialIntervalSemitones() const noexcept
{
    return initialIntervalSemitones_;
}

std::size_t PreparedDynamicPhaseVocoderPitchShifter::
maximumBlockFrames() const noexcept
{
    return maximumBlockFrames_;
}
}
