#include "talas/dsp/PreparedTimeFrequencyScale.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
constexpr auto tau = 2.0 * std::numbers::pi;

struct SourceInterpolationResponse { double gain, phase; };
SourceInterpolationResponse sourceInterpolationResponse(double omega, double start) noexcept
{
    const auto fraction = start - std::floor(start);
    const auto real = 1 + fraction * (std::cos(omega) - 1);
    const auto imaginary = fraction * std::sin(omega);
    return {std::hypot(real, imaginary), std::atan2(imaginary, real) - omega * fraction};
}

std::size_t validatedFrameSize(std::size_t size)
{
    if (size < 1024 || !isSupportedRadix2FftSize(size, Radix2FftSizeProfile::LongAnalysis))
        throw std::invalid_argument("Invalid TimeFrequencyScale analysis window");
    return size;
}
}

bool isSupportedTimeFrequencyScaleControls(double frequencyScale, double rate) noexcept
{
    return std::isfinite(frequencyScale) && frequencyScale > 0.0
        && std::isfinite(rate) && rate > 0.0;
}

std::size_t timeFrequencyScaleAnalysisFrameSize(
    double sampleRate, double minimumInputFrequency) noexcept
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0
        || !std::isfinite(minimumInputFrequency) || minimumInputFrequency <= 0.0)
        return 0;
    const auto required = sampleRate / minimumInputFrequency * 2.0;
    if (!std::isfinite(required) || required > maximumLongAnalysisFftSize)
        return 0;
    std::size_t size = 1024;
    while (static_cast<double>(size) < required) size *= 2;
    return size;
}

PreparedTimeFrequencyScale::PreparedTimeFrequencyScale(
    double sampleRate, std::size_t maximumBlockFrames, std::size_t analysisFrameSize,
    double initialFrequencyScale, double initialRate)
    : sampleRate_(sampleRate), maximumBlockFrames_(maximumBlockFrames),
      frameSize_(validatedFrameSize(analysisFrameSize)), hopSize_(frameSize_ / 4),
      historySize_(frameSize_ + hopSize_),
      initialFrequencyScale_(initialFrequencyScale), initialRate_(initialRate),
      fft_(frameSize_, Radix2FftSizeProfile::LongAnalysis),
      componentFft_(frameSize_ / 4, Radix2FftSizeProfile::LongAnalysis),
      componentAnalysis_(std::make_unique<IncrementalStationaryModel>()), componentRaw_(frameSize_), window_(frameSize_),
      clockHistory_(historySize_), frequencyHistory_(historySize_, initialFrequencyScale),
      neutralHistory_(historySize_, initialFrequencyScale == initialRate),
      rawHistory_(historySize_), overlap_(historySize_),
      previousPhase_(frameSize_ / 2 + 1), synthesisPhase_(frameSize_ / 2 + 1),
      analysis_(frameSize_), synthesis_(frameSize_), magnitude_(frameSize_ / 2 + 1),
      phase_(frameSize_ / 2 + 1), frequencyBin_(frameSize_ / 2 + 1), peaks_(frameSize_ / 2 + 1),
      currentFrequencyScale_(initialFrequencyScale), targetFrequencyScale_(initialFrequencyScale),
      spectralMix_(initialFrequencyScale == initialRate ? 0.0 : 1.0),
      targetSpectralMix_(spectralMix_)
{
    if (!std::isfinite(sampleRate_) || sampleRate_ <= 0.0 || maximumBlockFrames_ == 0
        || !isSupportedTimeFrequencyScaleControls(initialFrequencyScale, initialRate))
        throw std::invalid_argument("Invalid TimeFrequencyScale preparation");
    for (std::size_t i = 0; i < frameSize_; ++i)
        window_[i] = 0.5 - 0.5 * std::cos(tau * static_cast<double>(i) / frameSize_);
}

double PreparedTimeFrequencyScale::sourceSample(
    std::span<const double> source, double position) noexcept
{
    if (position < 0.0 || position > static_cast<double>(source.size() - 1)) return 0.0;
    const auto lower = static_cast<std::size_t>(position);
    const auto upper = std::min(lower + 1, source.size() - 1);
    const auto fraction = position - static_cast<double>(lower);
    return source[lower] * (1.0 - fraction) + source[upper] * fraction;
}

double PreparedTimeFrequencyScale::nextComponentSample() noexcept
{
    auto value = components_.oscillatorDc;
    for (std::size_t i = 0; i < components_.frame.count; ++i)
    {
        auto& oscillator = components_.oscillators[i];
        value += oscillator.amplitude * oscillator.sine;
        const auto sine = oscillator.sine * oscillator.stepCosine + oscillator.cosine * oscillator.stepSine;
        oscillator.cosine = oscillator.cosine * oscillator.stepCosine - oscillator.sine * oscillator.stepSine;
        oscillator.sine = sine;
    }
    return value;
}

void PreparedTimeFrequencyScale::advanceComponentAnalysis(std::size_t work) noexcept
{
    auto& job = *componentAnalysis_;
    if (job.status() != IncrementalStationaryModel::Status::Running) return;
    job.advance(componentFft_, work);
    if (job.status() == IncrementalStationaryModel::Status::Ready)
    {
        auto& result = components_.candidate;
        result.count = job.size(); result.sourceStart = components_.jobStart; result.dc = job.dc(); result.present = true;
        for (std::size_t i = 0; i < result.count; ++i)
        {
            result.frequency[i] = job.frequency(i);
            // The fitted window includes sourceSample's fractional linear
            // interpolation. Store the underlying source sinusoid so a later
            // fractional clock changes neither its identity nor output gain.
            const auto response = sourceInterpolationResponse(tau * result.frequency[i] / sampleRate_,
                result.sourceStart);
            result.amplitude[i] = std::hypot(job.cosineCoefficient(i), job.sineCoefficient(i)) / response.gain;
            result.phase[i] = std::atan2(job.cosineCoefficient(i), job.sineCoefficient(i)) - response.phase;
            result.present &= response.gain >= std::sqrt(std::numeric_limits<double>::epsilon())
                && std::isfinite(result.amplitude[i]) && std::isfinite(result.phase[i]);
        }
        components_.candidateReady = result.present;
    }
    if (job.status() != IncrementalStationaryModel::Status::Running) job.reset();
}

void PreparedTimeFrequencyScale::prepareComponentFrame(double start) noexcept
{
    auto& state = components_;
    // Existing component phases already describe the preceding synthesis center.
    for (std::size_t i = 0; i < state.active.count; ++i)
    {
        const auto step = (tau * state.active.frequency[i] / sampleRate_) * currentFrequencyScale_;
        state.outputPhase[i] = std::isfinite(step)
            ? std::remainder(state.outputPhase[i] + std::remainder(step, tau) * hopSize_, tau) : 0;
    }
    state.frameIsCandidate = state.candidateReady && !state.currentValidated;
    state.seedPhase.fill(false);
    state.frame = state.frameIsCandidate ? state.candidate : state.active;
    if (state.frameIsCandidate) state.candidateReady = false;
    state.mean = state.variance = state.errorEnergy = state.maxError = 0;
    state.badFit = false;
    if (!state.frame.present) { state.mix = 0; frameStage_ = FrameStage::Analysis; return; }
    // The recurrence predicts the current owned raw window from the original
    // source basis. No source pointer is consulted after capture.
    state.oscillatorDc = state.frame.dc / state.rawScale;
    for (std::size_t i = 0; i < state.frame.count; ++i)
    {
        const auto omega = tau * state.frame.frequency[i] / sampleRate_;
        const auto response = sourceInterpolationResponse(omega, start);
        const auto phase = std::remainder(state.frame.phase[i] + omega * (start - state.frame.sourceStart)
            + response.phase, tau);
        state.oscillators[i] = {std::sin(phase), std::cos(phase), std::sin(omega), std::cos(omega),
            state.frame.amplitude[i] / state.rawScale * response.gain};
    }
    frameStage_ = FrameStage::ValidateComponents;
}

void PreparedTimeFrequencyScale::finishComponentValidation() noexcept
{
    auto& state = components_;
    const auto floor = 1e-12 / state.rawScale;
    const auto normalizer = std::max(std::sqrt(std::max(0.0, state.variance) / frameSize_), floor);
    const auto valid = !state.badFit && (state.frame.count == 0 ? state.maxError < floor
        : std::sqrt(state.errorEnergy / frameSize_) / normalizer <= IncrementalSinusoidFit::residualLimit);
    if (valid && state.frameIsCandidate)
    {
        // Keep an existing synthesis phase only when frequency AND original
        // source phase identify the same component. A source phase jump must
        // not silently become an unrelated continuation.
        auto nextPhase = state.outputPhase;
        const auto& incoming = state.frame;
        for (std::size_t i = 0; i < incoming.count; ++i)
        {
            const auto newSourcePhase = std::remainder(incoming.phase[i]
                + tau * incoming.frequency[i] / sampleRate_ * (frameCenter_ - incoming.sourceStart), tau);
            nextPhase[i] = newSourcePhase;
            state.seedPhase[i] = true;
            for (std::size_t previous = 0; previous < state.active.count; ++previous)
            {
                const auto oldSourcePhase = std::remainder(state.active.phase[previous]
                    + tau * state.active.frequency[previous] / sampleRate_ * (frameCenter_ - state.active.sourceStart), tau);
                if (state.mix > 0 && std::abs(incoming.frequency[i] - state.active.frequency[previous]) <= .02
                    && std::abs(std::remainder(newSourcePhase - oldSourcePhase, tau)) <= 1e-3)
                { nextPhase[i] = state.outputPhase[previous]; state.seedPhase[i] = false; break; }
            }
        }
        state.active = incoming; state.outputPhase = nextPhase;
    }
    state.currentValidated = valid;
    if (valid) state.candidateReady = false; // A healthy current model needs no queued replacement.
    state.mix = state.active.present ? std::clamp(state.mix + (valid ? .25 : -.25), 0.0, 1.0) : 0;
    state.frame = state.active;
    state.oscillatorDc = state.frame.dc;
    for (std::size_t i = 0; i < state.frame.count; ++i)
    {
        const auto audible = state.frame.frequency[i] <= (sampleRate_ / 2) / currentFrequencyScale_;
        const auto omega = audible ? tau * state.frame.frequency[i] / sampleRate_ * currentFrequencyScale_ : 0;
        const auto phase = std::remainder(state.outputPhase[i] - omega * (frameSize_ / 2), tau);
        state.oscillators[i] = {std::sin(phase), std::cos(phase), std::sin(omega), std::cos(omega),
            audible ? state.frame.amplitude[i] : 0};
    }
    frameIndex_ = 0; frameStage_ = FrameStage::Analysis;
}

void PreparedTimeFrequencyScale::alignComponentPhase(std::size_t component) noexcept
{
    auto& state = components_;
    if (!state.seedPhase[component] || peakCount_ == 0
        || state.frame.frequency[component] > (sampleRate_ / 2) / currentFrequencyScale_) return;
    const auto sourceBin = state.frame.frequency[component] * frameSize_ / sampleRate_;
    auto nearest = std::lower_bound(peaks_.begin(), peaks_.begin() + peakCount_, sourceBin);
    if (nearest == peaks_.begin() + peakCount_) --nearest;
    else if (nearest != peaks_.begin() && sourceBin - *(nearest - 1) < *nearest - sourceBin) --nearest;
    const auto peak = *nearest;
    const auto mappedPeak = peak == 0 ? 0.0 : static_cast<double>(peak)
        + std::round((currentFrequencyScale_ - 1) * frequencyBin_[peak]);
    if (!std::isfinite(mappedPeak) || mappedPeak < 0 || mappedPeak > frameSize_ / 2) return;
    // The Hann lobe's center phase follows the source phase plus the legacy
    // peak's accumulated phase and integer bin translation. Seed only a new
    // component here, after the current spectral phase/peak stages. Otherwise
    // a crossfade between unrelated phases could cancel a healthy source.
    const auto sourceOmega = tau * state.frame.frequency[component] / sampleRate_;
    const auto response = sourceInterpolationResponse(sourceOmega, frameCenter_);
    state.outputPhase[component] = std::remainder(state.frame.phase[component]
        + sourceOmega * (frameCenter_ - state.frame.sourceStart) + response.phase
        + (peak == 0 ? 0 : synthesisPhase_[peak] - phase_[peak])
        + std::numbers::pi * (mappedPeak - peak), tau);
    const auto omega = sourceOmega * currentFrequencyScale_;
    const auto phase = std::remainder(state.outputPhase[component] - omega * (frameSize_ / 2), tau);
    state.oscillators[component].sine = std::sin(phase);
    state.oscillators[component].cosine = std::cos(phase);
}

PhaseVocoderPitchStatus PreparedTimeFrequencyScale::captureFrame(
    std::span<const double> source, double center, double frequencyScale) noexcept
{
    if (frameStage_ != FrameStage::Idle) return PhaseVocoderPitchStatus::InternalSpectralFailure;
    const auto start = center - static_cast<double>(frameSize_ / 2);
    const auto sourceHop = center - previousCenter_;
    frameIsFirst_ = !hasAnalysis_;
    // Use a local original-rate reference for ambiguous long advances and
    // sub-sample advances whose phase subtraction can lose precision (or whose
    // clock addition rounds to zero). Rate still controls only source position.
    frameUsesReference_ = hasAnalysis_
        && (sourceHop < 1.0 || sourceHop > static_cast<double>(hopSize_));
    framePhaseHop_ = frameUsesReference_ ? static_cast<double>(hopSize_) : sourceHop;
    frameCenter_ = center;
    // Capture both windows NOW. A live writer may replace their samples while
    // the subsequent FFT job is spread across calls; no source span is retained.
    components_.rawScale = 0;
    for (std::size_t i = 0; i < frameSize_; ++i)
    {
        const auto sample = sourceSample(source, start + static_cast<double>(i));
        if (!std::isfinite(sample)) return PhaseVocoderPitchStatus::NonFiniteInput;
        analysis_[i] = {sample * window_[i], 0.0};
        componentRaw_[i] = sample;
        components_.rawScale = std::max(components_.rawScale, std::abs(sample));
        if (frameUsesReference_)
        {
            const auto reference = sourceSample(source, start - static_cast<double>(hopSize_)
                + static_cast<double>(i));
            if (!std::isfinite(reference)) return PhaseVocoderPitchStatus::NonFiniteInput;
            synthesis_[i] = {reference * window_[i], 0.0};
        }
    }
    if (frequencyScale != targetFrequencyScale_)
    {
        targetFrequencyScale_ = frequencyScale;
        scaleRampHopsRemaining_ = phaseVocoderPitchRatioRampHops;
    }
    if (scaleRampHopsRemaining_ != 0)
        currentFrequencyScale_ += (targetFrequencyScale_ - currentFrequencyScale_)
            / static_cast<double>(scaleRampHopsRemaining_--);
    frameOutputIndex_ = (overlapReadIndex_ + hopSize_) % historySize_;
    frameSamplesRemaining_ = hopSize_;
    frameIndex_ = peakCount_ = peakIndex_ = 0;
    fftCursor_ = fft_.beginTransform(false);
    if (components_.rawScale == 0) components_.rawScale = 1;
    // A long analysis owns its own raw snapshot. Do not spend its startup work
    // on left/right padding, or replace a continuously validated active model.
    if (componentAnalysis_->status() != IncrementalStationaryModel::Status::Running
        && !components_.candidateReady && !components_.currentValidated
        && start >= 0 && start + frameSize_ - 1 <= static_cast<double>(source.size() - 1))
    {
        if (componentAnalysis_->begin(componentRaw_, sampleRate_)) components_.jobStart = start;
    }
    prepareComponentFrame(start);
    return PhaseVocoderPitchStatus::Ok;
}

PhaseVocoderPitchStatus PreparedTimeFrequencyScale::advanceFrame(std::size_t& work) noexcept
{
    // Phase/map bins cost16/8; a component recurrence costs one FFT-like
    // arithmetic unit. The caller shares the fixed320-unit total between this
    // frame and the source estimator. The frame allowance stays256 for at most8
    // components; the source estimator can use units left over by the frame.
    // Capture is a separate
    // bounded O(N) pass to preserve the exact live-source snapshot instant.
    const auto binCount = magnitude_.size();
    while (work != 0 && frameStage_ != FrameStage::Idle)
    {
        if (frameStage_ == FrameStage::ValidateComponents)
        {
            const auto cost = 2 + components_.frame.count;
            if (work < cost) break;
            work -= cost;
            const auto sample = componentRaw_[frameIndex_] / components_.rawScale;
            const auto difference = sample - components_.mean;
            components_.mean += difference / (frameIndex_ + 1);
            components_.variance += difference * (sample - components_.mean);
            const auto error = sample - nextComponentSample();
            components_.errorEnergy += error * error;
            components_.maxError = std::max(components_.maxError, std::abs(error));
            components_.badFit |= !std::isfinite(components_.errorEnergy) || !std::isfinite(error);
            if (++frameIndex_ == frameSize_) finishComponentValidation();
            continue;
        }
        if (frameStage_ == FrameStage::Analysis || frameStage_ == FrameStage::Reference
            || frameStage_ == FrameStage::Inverse)
        {
            auto& values = frameStage_ == FrameStage::Analysis ? analysis_ : synthesis_;
            if (!fft_.advanceTransform(values, fftCursor_, work))
                return PhaseVocoderPitchStatus::InternalSpectralFailure;
            if (!fftCursor_.complete()) break;
            if (frameStage_ == FrameStage::Analysis && frameUsesReference_)
            {
                frameStage_ = FrameStage::Reference;
                fftCursor_ = fft_.beginTransform(false);
            }
            else
            {
                frameStage_ = frameStage_ == FrameStage::Inverse ? FrameStage::Overlap : FrameStage::Phase;
                frameIndex_ = 0;
            }
            continue;
        }
        if (frameStage_ == FrameStage::Phase)
        {
            if (work < 16) break;
            work -= 16;
            const auto bin = frameIndex_++;
            magnitude_[bin] = std::abs(analysis_[bin]);
            phase_[bin] = std::arg(analysis_[bin]);
            if (!std::isfinite(magnitude_[bin])) return PhaseVocoderPitchStatus::NumericOverflow;
            const auto omega = tau * static_cast<double>(bin) / frameSize_;
            frequencyBin_[bin] = static_cast<double>(bin);
            if (!hasAnalysis_) synthesisPhase_[bin] = phase_[bin];
            else
            {
                if (frameUsesReference_ && (!std::isfinite(synthesis_[bin].real())
                    || !std::isfinite(synthesis_[bin].imag())))
                    return PhaseVocoderPitchStatus::NumericOverflow;
                const auto previousPhase = frameUsesReference_
                    ? std::arg(synthesis_[bin]) : previousPhase_[bin];
                const auto measured = framePhaseHop_ > 0.0
                    ? omega + std::remainder(phase_[bin] - previousPhase
                        - omega * framePhaseHop_, tau) / framePhaseHop_ : omega;
                frequencyBin_[bin] = measured * static_cast<double>(frameSize_) / tau;
                synthesisPhase_[bin] = std::remainder(synthesisPhase_[bin]
                    + measured * currentFrequencyScale_ * hopSize_, tau);
                if (!std::isfinite(synthesisPhase_[bin])) return PhaseVocoderPitchStatus::NumericOverflow;
            }
            previousPhase_[bin] = phase_[bin];
            if (frameIndex_ == binCount)
            {
                previousCenter_ = frameCenter_;
                hasAnalysis_ = true;
                frameIndex_ = 0;
                frameStage_ = FrameStage::Peaks;
            }
        }
        else if (frameStage_ == FrameStage::Peaks)
        {
            --work;
            const auto bin = frameIndex_++;
            if ((bin == 0 || magnitude_[bin] > magnitude_[bin - 1])
                && (bin + 1 == binCount || magnitude_[bin] >= magnitude_[bin + 1]))
                peaks_[peakCount_++] = bin;
            if (frameIndex_ == binCount)
            {
                frameIndex_ = 0;
                frameStage_ = components_.frame.count == 0 ? FrameStage::Clear : FrameStage::AlignComponents;
            }
        }
        else if (frameStage_ == FrameStage::AlignComponents)
        {
            if (work < 32) break;
            work -= 32;
            alignComponentPhase(frameIndex_++);
            if (frameIndex_ == components_.frame.count) { frameIndex_ = 0; frameStage_ = FrameStage::Clear; }
        }
        else if (frameStage_ == FrameStage::Clear)
        {
            --work;
            synthesis_[frameIndex_++] = {};
            if (frameIndex_ == frameSize_) { frameIndex_ = 0; frameStage_ = FrameStage::Map; }
        }
        else if (frameStage_ == FrameStage::Map)
        {
            if (work < 8) break;
            work -= 8;
            const auto bin = frameIndex_++;
            while (peakIndex_ + 1 < peakCount_
                && bin > (peaks_[peakIndex_] + peaks_[peakIndex_ + 1]) / 2) ++peakIndex_;
            const auto peak = peakCount_ == 0 ? bin : peaks_[peakIndex_];
            const auto mappedPeak = peak == 0 ? 0.0 : static_cast<double>(peak)
                + std::round((currentFrequencyScale_ - 1.0) * frequencyBin_[peak]);
            if (std::isfinite(mappedPeak) && mappedPeak >= 0.0
                && mappedPeak <= static_cast<double>(binCount - 1))
            {
                auto destination = static_cast<std::ptrdiff_t>(bin)
                    + static_cast<std::ptrdiff_t>(mappedPeak) - static_cast<std::ptrdiff_t>(peak);
                if (destination > -static_cast<std::ptrdiff_t>(binCount)
                    && destination < static_cast<std::ptrdiff_t>(binCount))
                {
                    auto value = peak == 0 ? analysis_[bin]
                        : std::polar(magnitude_[bin], synthesisPhase_[peak] + phase_[bin] - phase_[peak]);
                    // A translated positive-frequency lobe can cross DC.
                    // Store its negative part through conjugation so Mirror
                    // restores both halves instead of discarding that energy.
                    if (destination < 0)
                    {
                        destination = -destination;
                        value = std::conj(value);
                    }
                    const auto sourceEndpoint = bin == 0 || bin + 1 == binCount;
                    const auto destinationEndpoint = destination == 0
                        || destination == static_cast<std::ptrdiff_t>(binCount - 1);
                    // Interior coefficients represent conjugate pairs; DC and
                    // Nyquist each represent one real coefficient. Account for
                    // a pair merging at, or splitting from, either endpoint.
                    if (sourceEndpoint != destinationEndpoint)
                        value *= sourceEndpoint ? 0.5 : 2.0;
                    synthesis_[static_cast<std::size_t>(destination)] += value;
                }
            }
            if (frameIndex_ == binCount)
            {
                synthesis_[0] = {synthesis_[0].real(), 0.0};
                synthesis_[frameSize_ / 2] = {synthesis_[frameSize_ / 2].real(), 0.0};
                frameIndex_ = 1;
                frameStage_ = FrameStage::Mirror;
            }
        }
        else if (frameStage_ == FrameStage::Mirror)
        {
            --work;
            synthesis_[frameSize_ - frameIndex_] = std::conj(synthesis_[frameIndex_]);
            if (++frameIndex_ == frameSize_ / 2)
            {
                frameStage_ = FrameStage::Inverse;
                fftCursor_ = fft_.beginTransform(true);
            }
        }
        else if (frameStage_ == FrameStage::Overlap)
        {
            const auto cost = 2 + (components_.mix > 0 ? components_.frame.count : 0);
            if (work < cost) break;
            work -= cost;
            const auto index = (frameOutputIndex_ + frameIndex_) % historySize_;
            // The first frame replaces stale storage after reset. Every other
            // slot is consumed/cleared before a later frame can add to it.
            auto frameValue = synthesis_[frameIndex_].real();
            if (components_.mix > 0)
                frameValue = frameValue * (1 - components_.mix)
                    + nextComponentSample() * window_[frameIndex_] * components_.mix;
            const auto value = (frameIsFirst_ ? 0.0 : overlap_[index])
                + frameValue * window_[frameIndex_] / 1.5;
            if (!std::isfinite(value)) return PhaseVocoderPitchStatus::NumericOverflow;
            overlap_[index] = value;
            if (++frameIndex_ == frameSize_) frameStage_ = FrameStage::Idle;
        }
    }
    return PhaseVocoderPitchStatus::Ok;
}

PhaseVocoderPitchStatus PreparedTimeFrequencyScale::processSource(
    std::span<const double> source, double frequencyScale, double rate,
    SpectralFrameTimestamp blockStart, std::size_t compiledInputLatencyFrames,
    std::span<double> output) noexcept
{
    if (!isSupportedTimeFrequencyScaleControls(frequencyScale, rate))
        return PhaseVocoderPitchStatus::InvalidInterval;
    if (source.empty()) return PhaseVocoderPitchStatus::InvalidBufferShape;
    if (output.size() > maximumBlockFrames_) return PhaseVocoderPitchStatus::BlockCapacityExceeded;
    if (output.size() > std::numeric_limits<std::uint64_t>::max() - blockStart.absoluteFrame
        || output.size() > std::numeric_limits<std::uint64_t>::max() - processedFrames_)
        return PhaseVocoderPitchStatus::TimestampOverflow;
    if (hasTimeline_ && (blockStart.transportEpoch != nextBlock_.transportEpoch
        || blockStart.discontinuitySequence != nextBlock_.discontinuitySequence
        || blockStart.absoluteFrame != nextBlock_.absoluteFrame
        || compiledInputLatencyFrames != inputLatency_))
        return PhaseVocoderPitchStatus::DiscontinuousBlock;
    if (output.empty()) return PhaseVocoderPitchStatus::Ok;
    hasTimeline_ = true;
    inputLatency_ = compiledInputLatencyFrames;
    nextBlock_ = blockStart;
    for (auto& value : output)
    {
        const auto historyIndex = static_cast<std::size_t>(processedFrames_ % historySize_);
        clockHistory_[historyIndex] = sourcePosition_;
        frequencyHistory_[historyIndex] = frequencyScale;
        neutralHistory_[historyIndex] = frequencyScale == rate;
        rawHistory_[historyIndex] = sourceSample(source, sourcePosition_);
        if (!std::isfinite(rawHistory_[historyIndex])) return PhaseVocoderPitchStatus::NonFiniteInput;
        const auto sourceLimit = static_cast<double>(source.size()) + frameSize_;
        sourcePosition_ = rate >= sourceLimit - sourcePosition_
            ? sourceLimit : sourcePosition_ + rate;
        const auto untilCapture = processedFrames_ < frameSize_ - 1
            ? frameSize_ - 1 - processedFrames_
            : (hopSize_ - (processedFrames_ - (frameSize_ - 1)) % hopSize_) % hopSize_;
        if (untilCapture == 0)
        {
            // History entries contain the clock BEFORE integration for that sample.
            const auto centerIndex = (historyIndex + historySize_ - frameSize_ / 2 + 1) % historySize_;
            const auto status = captureFrame(source, clockHistory_[centerIndex],
                frequencyHistory_[centerIndex]);
            if (status != PhaseVocoderPitchStatus::Ok) return status;
        }
        auto modelWork = std::size_t{320};
        if (frameStage_ != FrameStage::Idle)
        {
            // Two N-sample recurrences per extra partial require8 more units
            // per output sample over H=N/4. Both jobs share the same total;
            // once the frame finishes the estimator can use all remaining work.
            const auto partials = std::max(components_.frame.count, components_.active.count);
            const auto frameAllowance = std::max(std::size_t{256}, 192 + 8 * partials);
            static_assert(192 + 8 * IncrementalStationaryModel::maximumPartials <= 320);
            auto frameWork = frameAllowance;
            const auto status = advanceFrame(frameWork);
            if (status != PhaseVocoderPitchStatus::Ok) return status;
            modelWork -= frameAllowance - frameWork;
            if (frameStage_ != FrameStage::Idle && --frameSamplesRemaining_ == 0)
                return PhaseVocoderPitchStatus::InternalSpectralFailure;
        }
        // Leave room for the atomic source capture in the next short interval.
        // Use sample position, so scheduling is independent of call partitions.
        // The original 64-unit estimator allowance applies near each capture;
        // elsewhere it can still use the unused portion of the 320-unit total.
        if (untilCapture < 32) modelWork = std::min(modelWork, std::size_t{64});
        advanceComponentAnalysis(modelWork);
        const auto delayedIndex = (historyIndex + 1) % historySize_;
        if (processedFrames_ >= latencyFrames())
        {
            const auto requestedMix = neutralHistory_[delayedIndex] ? 0.0 : 1.0;
            if (requestedMix != targetSpectralMix_)
            {
                targetSpectralMix_ = requestedMix;
                mixFramesRemaining_ = frameSize_;
                spectralMixStep_ = (targetSpectralMix_ - spectralMix_) / frameSize_;
            }
        }
        // Reset invalidates history logically. Do not read its old contents
        // until fresh controls, raw samples and the first OLA job are ready.
        value = processedFrames_ < latencyFrames() ? 0.0
            : rawHistory_[delayedIndex] * std::cos(std::numbers::pi * 0.5 * spectralMix_)
                + overlap_[overlapReadIndex_] * std::sin(std::numbers::pi * 0.5 * spectralMix_);
        if (!std::isfinite(value)) return PhaseVocoderPitchStatus::NumericOverflow;
        overlap_[overlapReadIndex_] = 0.0;
        overlapReadIndex_ = (overlapReadIndex_ + 1) % historySize_;
        if (mixFramesRemaining_ != 0 && --mixFramesRemaining_ == 0)
            spectralMix_ = targetSpectralMix_;
        else if (mixFramesRemaining_ != 0) spectralMix_ += spectralMixStep_;
        ++processedFrames_;
    }
    nextBlock_.absoluteFrame += output.size();
    return PhaseVocoderPitchStatus::Ok;
}

void PreparedTimeFrequencyScale::reset() noexcept
{
    // Histories are overwritten before their first delayed read, phase bins
    // are assigned by the first analysis, and its OLA replaces old overlap.
    // Keep reset constant-time even for one-sample live Gate segments.
    frameStage_ = FrameStage::Idle;
    fftCursor_ = {};
    frameIndex_ = peakCount_ = peakIndex_ = frameOutputIndex_ = frameSamplesRemaining_ = 0;
    frameCenter_ = framePhaseHop_ = 0.0;
    frameUsesReference_ = false;
    frameIsFirst_ = true;
    processedFrames_ = 0;
    overlapReadIndex_ = 0;
    sourcePosition_ = previousCenter_ = 0.0;
    hasAnalysis_ = hasTimeline_ = false;
    nextBlock_ = {};
    inputLatency_ = 0;
    currentFrequencyScale_ = targetFrequencyScale_ = initialFrequencyScale_;
    scaleRampHopsRemaining_ = 0;
    spectralMix_ = targetSpectralMix_ = initialFrequencyScale_ == initialRate_ ? 0.0 : 1.0;
    spectralMixStep_ = 0.0;
    mixFramesRemaining_ = 0;
    componentAnalysis_->reset();
    components_ = {};
}

bool PreparedTimeFrequencyScale::copyProcessingStateFrom(const PreparedTimeFrequencyScale& source) noexcept
{
    if (sampleRate_ != source.sampleRate_ || maximumBlockFrames_ != source.maximumBlockFrames_
        || frameSize_ != source.frameSize_ || initialFrequencyScale_ != source.initialFrequencyScale_
        || initialRate_ != source.initialRate_) return false;
    const auto copyPrefix = [](const auto& from, auto& to, std::size_t count) {
        std::copy_n(from.begin(), count, to.begin());
    };
    const auto copyRange = [](const auto& from, auto& to, std::size_t start, std::size_t count) {
        std::copy_n(from.begin() + start, count, to.begin() + start);
    };
    // Before the first ring wrap, untouched history is overwritten before any
    // read. Keep overlap in full: consumed zero slots also participate in later
    // addition when a checkpoint is restored into previously used storage.
    const auto historyFrames = source.processedFrames_ < historySize_
        ? static_cast<std::size_t>(source.processedFrames_) : historySize_;
    // Capture reads centers spaced H apart, N/2-1 samples behind capture.
    // Only the next two centers can already have been written; all remaining
    // clock/frequency entries are replaced before any future capture reads them.
    // Delayed neutral-path choice has its own compact boolean history.
    const auto copyCenter = [&](std::size_t index) {
        clockHistory_[index] = source.clockHistory_[index];
        frequencyHistory_[index] = source.frequencyHistory_[index];
    };
    if (source.processedFrames_ < frameSize_)
    {
        for (const auto center : {frameSize_ / 2, frameSize_ / 2 + hopSize_})
            if (center < source.processedFrames_) copyCenter(center);
    }
    else
    {
        const auto untilCapture = (hopSize_ - (source.processedFrames_ - (frameSize_ - 1)) % hopSize_) % hopSize_;
        const auto nextWrite = static_cast<std::size_t>(source.processedFrames_ % historySize_);
        for (const auto age : {2 * hopSize_ - 1 - untilCapture, hopSize_ - 1 - untilCapture})
            if (age != 0)
            {
                const auto index = (nextWrite + historySize_ - age) % historySize_;
                copyCenter(index);
            }
    }
    copyPrefix(source.neutralHistory_, neutralHistory_, historyFrames);
    copyPrefix(source.rawHistory_, rawHistory_, historyFrames);
    std::ranges::copy(source.overlap_, overlap_.begin());
    const auto bins = magnitude_.size();
    const auto phaseBins = source.hasAnalysis_ ? bins
        : source.frameStage_ == FrameStage::Phase ? source.frameIndex_ : 0;
    copyPrefix(source.previousPhase_, previousPhase_, phaseBins);
    copyPrefix(source.synthesisPhase_, synthesisPhase_, phaseBins);
    // A pending job owns only values which its remaining stages can read.
    // Omitted scratch is overwritten by those stages or the next capture.
    switch (source.frameStage_)
    {
    case FrameStage::Idle:
        break;
    case FrameStage::ValidateComponents:
        copyRange(source.componentRaw_, componentRaw_, source.frameIndex_, frameSize_ - source.frameIndex_);
        [[fallthrough]];
    case FrameStage::Analysis:
        copyPrefix(source.analysis_, analysis_, frameSize_);
        if (source.frameUsesReference_) copyPrefix(source.synthesis_, synthesis_, frameSize_);
        break;
    case FrameStage::Reference:
        copyPrefix(source.analysis_, analysis_, bins);
        copyPrefix(source.synthesis_, synthesis_, frameSize_);
        break;
    case FrameStage::Phase:
        copyPrefix(source.analysis_, analysis_, bins);
        if (source.frameUsesReference_)
            copyRange(source.synthesis_, synthesis_, source.frameIndex_, bins - source.frameIndex_);
        copyPrefix(source.magnitude_, magnitude_, source.frameIndex_);
        copyPrefix(source.phase_, phase_, source.frameIndex_);
        copyPrefix(source.frequencyBin_, frequencyBin_, source.frameIndex_);
        break;
    case FrameStage::Peaks:
    case FrameStage::AlignComponents:
    case FrameStage::Clear:
    case FrameStage::Map:
        copyPrefix(source.analysis_, analysis_, bins);
        copyPrefix(source.magnitude_, magnitude_, bins);
        copyPrefix(source.phase_, phase_, bins);
        copyPrefix(source.frequencyBin_, frequencyBin_, bins);
        copyPrefix(source.peaks_, peaks_, source.peakCount_);
        if (source.frameStage_ == FrameStage::Clear)
            copyPrefix(source.synthesis_, synthesis_, source.frameIndex_);
        if (source.frameStage_ == FrameStage::Map)
            copyPrefix(source.synthesis_, synthesis_, bins);
        break;
    case FrameStage::Mirror:
        copyPrefix(source.synthesis_, synthesis_, bins);
        copyRange(source.synthesis_, synthesis_, frameSize_ - source.frameIndex_ + 1,
            source.frameIndex_ - 1);
        break;
    case FrameStage::Inverse:
        copyPrefix(source.synthesis_, synthesis_, frameSize_);
        break;
    case FrameStage::Overlap:
        copyRange(source.synthesis_, synthesis_, source.frameIndex_, frameSize_ - source.frameIndex_);
        break;
    }
    frameStage_ = source.frameStage_;
    fftCursor_ = source.fftCursor_;
    frameIndex_ = source.frameIndex_;
    peakCount_ = source.peakCount_;
    peakIndex_ = source.peakIndex_;
    frameOutputIndex_ = source.frameOutputIndex_;
    frameSamplesRemaining_ = source.frameSamplesRemaining_;
    frameCenter_ = source.frameCenter_;
    framePhaseHop_ = source.framePhaseHop_;
    frameUsesReference_ = source.frameUsesReference_;
    frameIsFirst_ = source.frameIsFirst_;
    processedFrames_ = source.processedFrames_;
    overlapReadIndex_ = source.overlapReadIndex_;
    sourcePosition_ = source.sourcePosition_;
    previousCenter_ = source.previousCenter_;
    hasAnalysis_ = source.hasAnalysis_;
    hasTimeline_ = source.hasTimeline_;
    nextBlock_ = source.nextBlock_;
    inputLatency_ = source.inputLatency_;
    currentFrequencyScale_ = source.currentFrequencyScale_;
    targetFrequencyScale_ = source.targetFrequencyScale_;
    scaleRampHopsRemaining_ = source.scaleRampHopsRemaining_;
    spectralMix_ = source.spectralMix_;
    targetSpectralMix_ = source.targetSpectralMix_;
    spectralMixStep_ = source.spectralMixStep_;
    mixFramesRemaining_ = source.mixFramesRemaining_;
    components_ = source.components_;
    if (source.componentAnalysis_->status() == IncrementalStationaryModel::Status::Running)
        componentAnalysis_->copyProcessingStateFrom(*source.componentAnalysis_);
    else componentAnalysis_->reset();
    return true;
}

std::unique_ptr<PreparedTimeFrequencyScale> PreparedTimeFrequencyScale::cloneProcessingState() const
{
    auto result = std::make_unique<PreparedTimeFrequencyScale>(sampleRate_, maximumBlockFrames_,
        frameSize_, initialFrequencyScale_, initialRate_);
    if (!result->copyProcessingStateFrom(*this)) throw std::logic_error("TimeFrequencyScale snapshot mismatch");
    return result;
}

bool PreparedTimeFrequencyScale::retimeNextBlockAbsoluteFrame(std::uint64_t absoluteFrame) noexcept
{
    nextBlock_.absoluteFrame = absoluteFrame;
    return true;
}
}
