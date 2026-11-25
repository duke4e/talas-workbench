#include "talas/dsp/DiffuseSpectralStretch.h"

#include "talas/dsp/SpectralWindow.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
constexpr double twoPi = 2.0 * std::numbers::pi_v<double>;
// Synthesis frames use the periodic Hann window twice (analysis and
// synthesis) at a hop of N/4, so the overlapped window-squared sum is the
// constant 1.5 in steady state. Dividing by that constant lets the first
// and last frames fade in and out with the window instead of being
// amplified by the tiny accumulated weight at the frame edges.
constexpr double overlapNormalization = 1.5;

[[nodiscard]] bool inUnitInterval(double value) noexcept
{
    return std::isfinite(value) && value >= 0.0 && value <= 1.0;
}

[[nodiscard]] double wrapPhase(double value) noexcept
{
    return std::remainder(value, twoPi);
}

[[nodiscard]] std::uint64_t mix64(std::uint64_t value) noexcept
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] double phaseOffset(
    std::uint32_t seed,
    std::uint64_t frame,
    std::size_t bin) noexcept
{
    auto mixed = mix64(static_cast<std::uint64_t>(seed));
    mixed ^= mix64(frame + 0xd1b54a32d192ed03ULL);
    mixed ^= mix64(static_cast<std::uint64_t>(bin)
        + 0x94d049bb133111ebULL);
    const auto mantissa = mix64(mixed) >> 11U;
    constexpr double inverse53 = 1.0 / 9007199254740992.0;
    return (static_cast<double>(mantissa) * inverse53 * twoPi)
        - std::numbers::pi_v<double>;
}

}

DiffuseSpectralStretchProcessingState::DiffuseSpectralStretchProcessingState(
    std::size_t frameSize)
    : analysisSpectrum(frameSize), synthesisSpectrum(frameSize),
      previousAnalysisPhases(frameSize / 2 + 1),
      synthesisPhases(frameSize / 2 + 1), magnitudeMemory(frameSize / 2 + 1),
      candidateAnalysisPhases(frameSize / 2 + 1),
      candidateSynthesisPhases(frameSize / 2 + 1),
      candidateMagnitudes(frameSize / 2 + 1), overlapSamples(frameSize),
      overlapWeights(frameSize)
{
}

bool isValidDiffuseSpectralStretchConfiguration(
    const DiffuseSpectralStretchConfiguration& configuration,
    std::span<const double> source) noexcept
{
    return std::isfinite(configuration.sampleRate)
        && configuration.sampleRate >= 8000.0
        && configuration.sampleRate <= 384000.0
        && std::isfinite(configuration.stretch)
        && configuration.stretch >= diffuseSpectralStretchMinimumFactor
        && configuration.stretch <= diffuseSpectralStretchMaximumFactor
        && inUnitInterval(configuration.smear)
        && inUnitInterval(configuration.diffusion)
        && !source.empty()
        && source.size() <= diffuseSpectralStretchMaximumSourceSamples
        && std::ranges::all_of(source, [](double sample) {
               return std::isfinite(sample);
           });
}

PreparedDiffuseSpectralStretch::PreparedDiffuseSpectralStretch(
    DiffuseSpectralStretchConfiguration configuration,
    std::span<const double> source)
    : configuration_(configuration),
      source_(source.begin(), source.end()),
      plan_(diffuseSpectralStretchFrameSize),
      window_(makeSpectralWindow(
          SpectralWindowKind::Hann,
          diffuseSpectralStretchFrameSize)),
      analysisSpectrum_(diffuseSpectralStretchFrameSize),
      synthesisSpectrum_(diffuseSpectralStretchFrameSize),
      previousAnalysisPhases_(diffuseSpectralStretchFrameSize / 2 + 1),
      synthesisPhases_(diffuseSpectralStretchFrameSize / 2 + 1),
      magnitudeMemory_(diffuseSpectralStretchFrameSize / 2 + 1),
      candidateAnalysisPhases_(diffuseSpectralStretchFrameSize / 2 + 1),
      candidateSynthesisPhases_(diffuseSpectralStretchFrameSize / 2 + 1),
      candidateMagnitudes_(diffuseSpectralStretchFrameSize / 2 + 1),
      overlapSamples_(diffuseSpectralStretchFrameSize),
      overlapWeights_(diffuseSpectralStretchFrameSize)
{
    if (!isValidDiffuseSpectralStretchConfiguration(configuration_, source_))
    {
        throw std::invalid_argument(
            "Invalid diffuse spectral stretch configuration");
    }
}

bool PreparedDiffuseSpectralStretch::processSample(
    double resetValue,
    double gate,
    double level,
    double& output) noexcept
{
    if (!std::isfinite(resetValue) || !std::isfinite(gate)
        || !std::isfinite(level) || level < 0.0 || level > 4.0)
    {
        return false;
    }

    const auto resetHigh = resetValue > 0.5;
    if (resetHigh && !previousResetHigh_)
        clearProcessingState();
    previousResetHigh_ = resetHigh;

    if (gate <= 0.0)
    {
        if (gateActive_)
            clearProcessingState();
        gateActive_ = false;
        output = 0.0;
        return true;
    }
    gateActive_ = true;

    if (startupFramesRemaining_ != 0)
    {
        --startupFramesRemaining_;
        output = 0.0;
        return true;
    }

    auto reachedSourceEnd = false;
    if (hopFramesRemaining_ == 0 && !sourceExhausted_)
    {
        const auto finalCursor = static_cast<double>(source_.size() - 1)
            + static_cast<double>(diffuseSpectralStretchFrameSize) * 0.5;
        if (analysisCursor_ > finalCursor)
            reachedSourceEnd = true;
        else
        {
            if (!generateFrame(level))
                return false;
            hopFramesRemaining_ = diffuseSpectralStretchHopSize;
        }
    }

    const auto candidateOutput =
        level * overlapSamples_[overlapReadIndex_] / overlapNormalization;
    if (!std::isfinite(candidateOutput))
        return false;

    if (reachedSourceEnd)
    {
        sourceExhausted_ = true;
        tailFramesRemaining_ = diffuseSpectralStretchFrameSize;
    }
    overlapSamples_[overlapReadIndex_] = 0.0;
    overlapWeights_[overlapReadIndex_] = 0.0;
    overlapReadIndex_ = (overlapReadIndex_ + 1)
        % diffuseSpectralStretchFrameSize;
    if (hopFramesRemaining_ != 0)
        --hopFramesRemaining_;
    if (sourceExhausted_ && tailFramesRemaining_ != 0)
        --tailFramesRemaining_;
    output = candidateOutput;
    return true;
}

bool PreparedDiffuseSpectralStretch::reconfigure(
    double stretch, double smear, double diffusion, double seed) noexcept
{
    auto candidate = configuration_;
    candidate.stretch = stretch;
    candidate.smear = smear;
    candidate.diffusion = diffusion;
    if (!std::isfinite(seed) || seed < 0.0
        || seed > static_cast<double>(std::numeric_limits<std::uint32_t>::max())
        || std::floor(seed) != seed
        || !isValidDiffuseSpectralStretchConfiguration(candidate, source_))
    {
        return false;
    }
    candidate.seed = static_cast<std::uint32_t>(seed);
    configuration_ = candidate;
    reset();
    return true;
}

void PreparedDiffuseSpectralStretch::reset() noexcept
{
    clearProcessingState();
    previousResetHigh_ = false;
    gateActive_ = false;
}

const DiffuseSpectralStretchConfiguration&
PreparedDiffuseSpectralStretch::configuration() const noexcept
{
    return configuration_;
}

std::size_t PreparedDiffuseSpectralStretch::sourceSampleCount() const noexcept
{
    return source_.size();
}

std::uint64_t PreparedDiffuseSpectralStretch::synthesisFrameCount() const noexcept
{
    return synthesisFrameCount_;
}

double PreparedDiffuseSpectralStretch::analysisCursor() const noexcept
{
    return analysisCursor_;
}

bool PreparedDiffuseSpectralStretch::exhausted() const noexcept
{
    return sourceExhausted_ && tailFramesRemaining_ == 0;
}

bool PreparedDiffuseSpectralStretch::
synthesisSpectrumIsExactlyHermitian() const noexcept
{
    return lastSynthesisSpectrumWasExactlyHermitian_;
}

DiffuseSpectralStretchProcessingState
PreparedDiffuseSpectralStretch::makeProcessingStateCheckpoint() const
{
    return DiffuseSpectralStretchProcessingState{
        diffuseSpectralStretchFrameSize};
}

bool PreparedDiffuseSpectralStretch::captureProcessingState(
    DiffuseSpectralStretchProcessingState& state) const noexcept
{
    constexpr auto binCount = diffuseSpectralStretchFrameSize / 2 + 1;
    if (state.analysisSpectrum.size() != diffuseSpectralStretchFrameSize
        || state.synthesisSpectrum.size() != diffuseSpectralStretchFrameSize
        || state.previousAnalysisPhases.size() != binCount
        || state.synthesisPhases.size() != binCount
        || state.magnitudeMemory.size() != binCount
        || state.candidateAnalysisPhases.size() != binCount
        || state.candidateSynthesisPhases.size() != binCount
        || state.candidateMagnitudes.size() != binCount
        || state.overlapSamples.size() != diffuseSpectralStretchFrameSize
        || state.overlapWeights.size() != diffuseSpectralStretchFrameSize)
    {
        return false;
    }
    std::ranges::copy(analysisSpectrum_, state.analysisSpectrum.begin());
    std::ranges::copy(synthesisSpectrum_, state.synthesisSpectrum.begin());
    std::ranges::copy(
        previousAnalysisPhases_, state.previousAnalysisPhases.begin());
    std::ranges::copy(synthesisPhases_, state.synthesisPhases.begin());
    std::ranges::copy(magnitudeMemory_, state.magnitudeMemory.begin());
    std::ranges::copy(
        candidateAnalysisPhases_, state.candidateAnalysisPhases.begin());
    std::ranges::copy(
        candidateSynthesisPhases_, state.candidateSynthesisPhases.begin());
    std::ranges::copy(candidateMagnitudes_, state.candidateMagnitudes.begin());
    std::ranges::copy(overlapSamples_, state.overlapSamples.begin());
    std::ranges::copy(overlapWeights_, state.overlapWeights.begin());
    state.analysisCursor = analysisCursor_;
    state.overlapReadIndex = overlapReadIndex_;
    state.hopFramesRemaining = hopFramesRemaining_;
    state.startupFramesRemaining = startupFramesRemaining_;
    state.tailFramesRemaining = tailFramesRemaining_;
    state.synthesisFrameCount = synthesisFrameCount_;
    state.phaseInitialized = phaseInitialized_;
    state.lastSynthesisSpectrumWasExactlyHermitian =
        lastSynthesisSpectrumWasExactlyHermitian_;
    state.sourceExhausted = sourceExhausted_;
    state.previousResetHigh = previousResetHigh_;
    state.gateActive = gateActive_;
    return true;
}

bool PreparedDiffuseSpectralStretch::restoreProcessingState(
    const DiffuseSpectralStretchProcessingState& state) noexcept
{
    constexpr auto binCount = diffuseSpectralStretchFrameSize / 2 + 1;
    if (state.analysisSpectrum.size() != diffuseSpectralStretchFrameSize
        || state.synthesisSpectrum.size() != diffuseSpectralStretchFrameSize
        || state.previousAnalysisPhases.size() != binCount
        || state.synthesisPhases.size() != binCount
        || state.magnitudeMemory.size() != binCount
        || state.candidateAnalysisPhases.size() != binCount
        || state.candidateSynthesisPhases.size() != binCount
        || state.candidateMagnitudes.size() != binCount
        || state.overlapSamples.size() != diffuseSpectralStretchFrameSize
        || state.overlapWeights.size() != diffuseSpectralStretchFrameSize
        || !std::isfinite(state.analysisCursor) || state.analysisCursor < 0.0
        || state.overlapReadIndex >= diffuseSpectralStretchFrameSize
        || state.hopFramesRemaining > diffuseSpectralStretchHopSize
        || state.startupFramesRemaining > diffuseSpectralStretchLatencyFrames
        || state.tailFramesRemaining > diffuseSpectralStretchFrameSize)
    {
        return false;
    }
    std::ranges::copy(state.analysisSpectrum, analysisSpectrum_.begin());
    std::ranges::copy(state.synthesisSpectrum, synthesisSpectrum_.begin());
    std::ranges::copy(
        state.previousAnalysisPhases, previousAnalysisPhases_.begin());
    std::ranges::copy(state.synthesisPhases, synthesisPhases_.begin());
    std::ranges::copy(state.magnitudeMemory, magnitudeMemory_.begin());
    std::ranges::copy(
        state.candidateAnalysisPhases, candidateAnalysisPhases_.begin());
    std::ranges::copy(
        state.candidateSynthesisPhases, candidateSynthesisPhases_.begin());
    std::ranges::copy(state.candidateMagnitudes, candidateMagnitudes_.begin());
    std::ranges::copy(state.overlapSamples, overlapSamples_.begin());
    std::ranges::copy(state.overlapWeights, overlapWeights_.begin());
    analysisCursor_ = state.analysisCursor;
    overlapReadIndex_ = state.overlapReadIndex;
    hopFramesRemaining_ = state.hopFramesRemaining;
    startupFramesRemaining_ = state.startupFramesRemaining;
    tailFramesRemaining_ = state.tailFramesRemaining;
    synthesisFrameCount_ = state.synthesisFrameCount;
    phaseInitialized_ = state.phaseInitialized;
    lastSynthesisSpectrumWasExactlyHermitian_ =
        state.lastSynthesisSpectrumWasExactlyHermitian;
    sourceExhausted_ = state.sourceExhausted;
    previousResetHigh_ = state.previousResetHigh;
    gateActive_ = state.gateActive;
    return true;
}

bool PreparedDiffuseSpectralStretch::generateFrame(double level) noexcept
{
    if (synthesisFrameCount_
        == std::numeric_limits<std::uint64_t>::max())
    {
        return false;
    }
    constexpr auto binCount = diffuseSpectralStretchFrameSize / 2 + 1;
    const auto halfFrame = static_cast<double>(
        diffuseSpectralStretchFrameSize) * 0.5;
    for (std::size_t index = 0;
         index < diffuseSpectralStretchFrameSize;
         ++index)
    {
        const auto sourcePosition = analysisCursor_
            + static_cast<double>(index) - halfFrame;
        const auto sample = sampleSource(sourcePosition) * window_[index];
        if (!std::isfinite(sample))
            return false;
        analysisSpectrum_[index] = {sample, 0.0};
    }
    if (!plan_.forward(analysisSpectrum_))
        return false;

    const auto inputHop = static_cast<double>(
        diffuseSpectralStretchHopSize) / configuration_.stretch;
    const auto nextAnalysisCursor = analysisCursor_ + inputHop;
    if (!std::isfinite(nextAnalysisCursor)
        || nextAnalysisCursor <= analysisCursor_)
    {
        return false;
    }
    for (std::size_t bin = 0; bin < binCount; ++bin)
    {
        const auto magnitude = std::abs(analysisSpectrum_[bin]);
        const auto analysisPhase = std::arg(analysisSpectrum_[bin]);
        const auto rememberedMagnitude = phaseInitialized_
            ? magnitudeMemory_[bin]
            : magnitude;
        const auto candidateMagnitude = std::lerp(
            magnitude, rememberedMagnitude, configuration_.smear);
        auto candidatePhase = analysisPhase;
        if (phaseInitialized_)
        {
            const auto expectedAnalysisAdvance = twoPi
                * static_cast<double>(bin) * inputHop
                / static_cast<double>(diffuseSpectralStretchFrameSize);
            const auto phaseDeviation = wrapPhase(
                analysisPhase - previousAnalysisPhases_[bin]
                - expectedAnalysisAdvance);
            const auto expectedSynthesisAdvance = twoPi
                * static_cast<double>(bin)
                * static_cast<double>(diffuseSpectralStretchHopSize)
                / static_cast<double>(diffuseSpectralStretchFrameSize);
            candidatePhase = synthesisPhases_[bin]
                + expectedSynthesisAdvance
                + phaseDeviation * configuration_.stretch;
        }
        candidatePhase = wrapPhase(candidatePhase
            + configuration_.diffusion * phaseOffset(
                configuration_.seed, synthesisFrameCount_, bin));
        if (!std::isfinite(magnitude) || !std::isfinite(analysisPhase)
            || !std::isfinite(candidateMagnitude)
            || candidateMagnitude < 0.0 || !std::isfinite(candidatePhase))
        {
            return false;
        }
        candidateAnalysisPhases_[bin] = analysisPhase;
        candidateSynthesisPhases_[bin] = candidatePhase;
        candidateMagnitudes_[bin] = candidateMagnitude;
    }

    std::ranges::fill(synthesisSpectrum_, std::complex<double>{});
    for (std::size_t bin = 0; bin < binCount; ++bin)
    {
        auto value = std::polar(
            candidateMagnitudes_[bin], candidateSynthesisPhases_[bin]);
        if (bin == 0 || bin == diffuseSpectralStretchFrameSize / 2)
            value.imag(0.0);
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
            return false;
        synthesisSpectrum_[bin] = value;
        if (bin != 0 && bin != diffuseSpectralStretchFrameSize / 2)
            synthesisSpectrum_[diffuseSpectralStretchFrameSize - bin]
                = std::conj(value);
    }
    auto spectrumExactlyHermitian = synthesisSpectrum_.front().imag() == 0.0
        && synthesisSpectrum_[diffuseSpectralStretchFrameSize / 2].imag()
            == 0.0;
    for (std::size_t bin = 1;
         spectrumExactlyHermitian
             && bin < diffuseSpectralStretchFrameSize / 2;
         ++bin)
    {
        spectrumExactlyHermitian =
            synthesisSpectrum_[diffuseSpectralStretchFrameSize - bin]
            == std::conj(synthesisSpectrum_[bin]);
    }
    if (!spectrumExactlyHermitian)
        return false;
    if (!plan_.inverse(synthesisSpectrum_))
        return false;

    for (std::size_t index = 0;
         index < diffuseSpectralStretchFrameSize;
         ++index)
    {
        const auto frameSample = synthesisSpectrum_[index].real()
            * window_[index];
        const auto weight = window_[index] * window_[index];
        const auto ringIndex = (overlapReadIndex_ + index)
            % diffuseSpectralStretchFrameSize;
        const auto candidateSample = overlapSamples_[ringIndex] + frameSample;
        const auto candidateWeight = overlapWeights_[ringIndex] + weight;
        if (!std::isfinite(frameSample) || !std::isfinite(weight)
            || !std::isfinite(candidateSample)
            || !std::isfinite(candidateWeight) || candidateWeight < 0.0)
        {
            return false;
        }
        if (index == 0
            && !std::isfinite(level * candidateSample / overlapNormalization))
        {
            return false;
        }
    }

    for (std::size_t index = 0;
         index < diffuseSpectralStretchFrameSize;
         ++index)
    {
        const auto ringIndex = (overlapReadIndex_ + index)
            % diffuseSpectralStretchFrameSize;
        overlapSamples_[ringIndex] += synthesisSpectrum_[index].real()
            * window_[index];
        overlapWeights_[ringIndex] += window_[index] * window_[index];
    }
    std::ranges::copy(
        candidateAnalysisPhases_, previousAnalysisPhases_.begin());
    std::ranges::copy(candidateSynthesisPhases_, synthesisPhases_.begin());
    std::ranges::copy(candidateMagnitudes_, magnitudeMemory_.begin());
    phaseInitialized_ = true;
    lastSynthesisSpectrumWasExactlyHermitian_ = true;
    analysisCursor_ = nextAnalysisCursor;
    ++synthesisFrameCount_;
    return true;
}

double PreparedDiffuseSpectralStretch::sampleSource(double position) const noexcept
{
    if (!std::isfinite(position) || position < 0.0)
        return 0.0;
    const auto last = static_cast<double>(source_.size() - 1);
    if (position > last + 1.0)
        return 0.0;
    const auto leftFloor = std::floor(position);
    if (leftFloor < 0.0 || leftFloor > last)
        return 0.0;
    const auto left = static_cast<std::size_t>(leftFloor);
    const auto fraction = position - leftFloor;
    const auto rightValue = left + 1 < source_.size()
        ? source_[left + 1]
        : 0.0;
    return std::lerp(source_[left], rightValue, fraction);
}

void PreparedDiffuseSpectralStretch::clearProcessingState() noexcept
{
    std::ranges::fill(analysisSpectrum_, std::complex<double>{});
    std::ranges::fill(synthesisSpectrum_, std::complex<double>{});
    std::ranges::fill(previousAnalysisPhases_, 0.0);
    std::ranges::fill(synthesisPhases_, 0.0);
    std::ranges::fill(magnitudeMemory_, 0.0);
    std::ranges::fill(candidateAnalysisPhases_, 0.0);
    std::ranges::fill(candidateSynthesisPhases_, 0.0);
    std::ranges::fill(candidateMagnitudes_, 0.0);
    std::ranges::fill(overlapSamples_, 0.0);
    std::ranges::fill(overlapWeights_, 0.0);
    analysisCursor_ = 0.0;
    overlapReadIndex_ = 0;
    hopFramesRemaining_ = 0;
    startupFramesRemaining_ = diffuseSpectralStretchLatencyFrames;
    tailFramesRemaining_ = 0;
    synthesisFrameCount_ = 0;
    phaseInitialized_ = false;
    lastSynthesisSpectrumWasExactlyHermitian_ = true;
    sourceExhausted_ = false;
}
}
