#include "talas/dsp/NativeStft.h"

#include "talas/dsp/SpectralWindow.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace talas::dsp
{
namespace
{
[[nodiscard]] std::size_t validatedFrameSize(
    const NativeStftConfiguration& configuration)
{
    if (!isValidNativeStftConfiguration(configuration))
        throw std::invalid_argument("Unsupported native STFT configuration");
    return configuration.frameSize;
}

[[nodiscard]] std::size_t checkedStorageSize(
    std::size_t frameSize,
    std::size_t frameCapacity)
{
    if (frameCapacity == 0
        || frameSize > std::numeric_limits<std::size_t>::max()
            / frameCapacity)
    {
        throw std::invalid_argument(
            "Native inverse STFT frame scratch capacity is invalid");
    }
    return frameSize * frameCapacity;
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

[[nodiscard]] bool materiallyNonReal(
    const std::complex<double>& value) noexcept
{
    const auto tolerance = 64.0 * std::numeric_limits<double>::epsilon()
        * std::max(1.0, std::abs(value.real()));
    return std::abs(value.imag()) > tolerance;
}
}

bool isValidNativeStftConfiguration(
    const NativeStftConfiguration& configuration) noexcept
{
    return isSupportedNativeFrameConfiguration(
        configuration.frameSize,
        configuration.hopSize,
        configuration.window);
}

PreparedForwardStft::PreparedForwardStft(
    NativeStftConfiguration configuration)
    : configuration_(configuration),
      plan_(validatedFrameSize(configuration)),
      window_(makeSpectralWindow(
          configuration.window,
          configuration.frameSize)),
      history_(configuration.frameSize, 0.0),
      transformScratch_(configuration.frameSize)
{
    const auto cola = validateConstantOverlapAdd(
        window_, configuration_.hopSize);
    if (!cola.valid)
        throw std::invalid_argument("Native STFT configuration failed COLA validation");
}

NativeStftStatus PreparedForwardStft::process(
    std::span<const double> input,
    SpectralFrameTimestamp blockStart,
    PreparedComplexSpectrumSequence& output,
    std::size_t compiledInputLatencyFrames) noexcept
{
    if (output.maximumBinsPerFrame()
            != configuration_.frameSize / 2 + 1
        || output.channelCount() != 1)
    {
        return NativeStftStatus::OutputSequenceRejected;
    }
    if (!output.frames().empty())
        return NativeStftStatus::OutputSequenceNotEmpty;
    if (initialized_)
    {
        if (blockStart.transportEpoch != segmentStart_.transportEpoch
            || blockStart.discontinuitySequence
                != segmentStart_.discontinuitySequence
            || blockStart.absoluteFrame != nextBlockAbsoluteFrame_
            || compiledInputLatencyFrames
                != preparedInputLatencyFrames_)
        {
            return NativeStftStatus::DiscontinuousBlock;
        }
    }

    std::uint64_t blockEnd = 0;
    std::uint64_t phaseStartAbsoluteFrame = 0;
    if (!addWithoutOverflow(
            blockStart.absoluteFrame,
            static_cast<std::uint64_t>(input.size()),
            blockEnd)
        || !addWithoutOverflow(
            initialized_
                ? segmentStart_.absoluteFrame
                : blockStart.absoluteFrame,
            static_cast<std::uint64_t>(compiledInputLatencyFrames),
            phaseStartAbsoluteFrame))
    {
        return NativeStftStatus::TimestampOverflow;
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
    std::uint64_t newSampleCount = 0;
    if (!addWithoutOverflow(
            samplesSinceReset_,
            static_cast<std::uint64_t>(sourceInputSize),
            newSampleCount))
    {
        return NativeStftStatus::TimestampOverflow;
    }
    const auto safeInputMagnitude = std::numeric_limits<double>::max()
        / static_cast<double>(configuration_.frameSize);
    if (!std::ranges::all_of(input, [&](double value) {
            return std::isfinite(value)
                && std::abs(value) <= safeInputMagnitude;
        }))
    {
        return std::ranges::all_of(input, [](double value) {
            return std::isfinite(value);
        })
            ? NativeStftStatus::NumericOverflow
            : NativeStftStatus::NonFiniteInput;
    }

    const auto oldFrameCount = emittedFrameCount(
        samplesSinceReset_,
        configuration_.frameSize,
        configuration_.hopSize);
    const auto newFrameCount = emittedFrameCount(
        newSampleCount,
        configuration_.frameSize,
        configuration_.hopSize);
    const auto framesToEmit = newFrameCount - oldFrameCount;
    if (framesToEmit > output.capacity())
        return NativeStftStatus::OutputCapacityExceeded;
    if (framesToEmit
        > std::numeric_limits<std::uint64_t>::max() - sourceSequence_)
    {
        return NativeStftStatus::TimestampOverflow;
    }

    if (!initialized_)
    {
        segmentStart_ = blockStart;
        preparedInputLatencyFrames_ = compiledInputLatencyFrames;
        nextBlockAbsoluteFrame_ = blockStart.absoluteFrame;
        initialized_ = true;
    }

    for (std::size_t inputIndex = sourceInputOffset;
         inputIndex < input.size();
         ++inputIndex)
    {
        history_[historyWriteIndex_] = input[inputIndex];
        historyWriteIndex_ = (historyWriteIndex_ + 1)
            % configuration_.frameSize;
        ++samplesSinceReset_;
        if (samplesSinceReset_ < configuration_.frameSize
            || (samplesSinceReset_ - configuration_.frameSize)
                % configuration_.hopSize != 0)
        {
            continue;
        }

        for (std::size_t frame = 0;
             frame < configuration_.frameSize;
             ++frame)
        {
            transformScratch_[frame] = {
                history_[(historyWriteIndex_ + frame)
                    % configuration_.frameSize]
                    * window_[frame],
                0.0,
            };
        }
        if (!plan_.forward(transformScratch_))
            return NativeStftStatus::OutputSequenceRejected;
        transformScratch_.front().imag(0.0);
        transformScratch_[configuration_.frameSize / 2].imag(0.0);

        std::uint64_t analysisAbsoluteFrame = 0;
        if (!addWithoutOverflow(
                segmentStart_.absoluteFrame,
                samplesSinceReset_
                    - static_cast<std::uint64_t>(
                        configuration_.frameSize),
                analysisAbsoluteFrame))
        {
            return NativeStftStatus::TimestampOverflow;
        }
        const SpectralFrameTimestamp analysisTimestamp{
            segmentStart_.transportEpoch,
            segmentStart_.discontinuitySequence,
            analysisAbsoluteFrame,
        };
        const auto status = output.append(
            analysisTimestamp,
            sourceSequence_,
            configuration_.frameSize,
            configuration_.hopSize,
            configuration_.window,
            0,
            std::span<const std::complex<double>>{transformScratch_}
                .first(configuration_.frameSize / 2 + 1));
        if (status != SpectralFrameStatus::Ok)
            return NativeStftStatus::OutputSequenceRejected;
        ++sourceSequence_;
    }

    nextBlockAbsoluteFrame_ = blockEnd;
    return NativeStftStatus::Ok;
}

void PreparedForwardStft::reset() noexcept
{
    std::ranges::fill(history_, 0.0);
    std::ranges::fill(transformScratch_, std::complex<double>{});
    historyWriteIndex_ = 0;
    samplesSinceReset_ = 0;
    sourceSequence_ = 0;
    segmentStart_ = {};
    nextBlockAbsoluteFrame_ = 0;
    preparedInputLatencyFrames_ = 0;
    initialized_ = false;
}

bool PreparedForwardStft::copyProcessingStateFrom(
    const PreparedForwardStft& source) noexcept
{
    if (configuration_ != source.configuration_
        || history_.size() != source.history_.size()
        || transformScratch_.size()
            != source.transformScratch_.size())
    {
        return false;
    }

    std::ranges::copy(source.history_, history_.begin());
    std::ranges::copy(
        source.transformScratch_, transformScratch_.begin());
    historyWriteIndex_ = source.historyWriteIndex_;
    samplesSinceReset_ = source.samplesSinceReset_;
    sourceSequence_ = source.sourceSequence_;
    segmentStart_ = source.segmentStart_;
    nextBlockAbsoluteFrame_ = source.nextBlockAbsoluteFrame_;
    preparedInputLatencyFrames_ = source.preparedInputLatencyFrames_;
    initialized_ = source.initialized_;
    return true;
}

bool PreparedForwardStft::retimeNextBlockAbsoluteFrame(
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
    segmentStart_.absoluteFrame += delta;
    nextBlockAbsoluteFrame_ = absoluteFrame;
    return true;
}

const NativeStftConfiguration&
PreparedForwardStft::configuration() const noexcept
{
    return configuration_;
}

PreparedInverseStft::PreparedInverseStft(
    NativeStftConfiguration configuration,
    std::size_t maximumFramesPerProcess,
    InverseStftSynthesisWindowPolicy synthesisWindowPolicy)
    : configuration_(configuration),
      plan_(validatedFrameSize(configuration)),
      window_(makeSpectralWindow(
          configuration.window,
          configuration.frameSize)),
      transformedFrames_(checkedStorageSize(
          configuration.frameSize,
          maximumFramesPerProcess)),
      overlapSamples_(configuration.frameSize, 0.0),
      overlapWeights_(configuration.frameSize, 0.0),
      preflightOverlapSamples_(configuration.frameSize, 0.0),
      preflightOverlapWeights_(configuration.frameSize, 0.0),
      maximumFramesPerProcess_(maximumFramesPerProcess),
      synthesisWindowPolicy_(synthesisWindowPolicy)
{
    auto normalizationWindow = window_;
    if (synthesisWindowPolicy_
        == InverseStftSynthesisWindowPolicy::ApplyAnalysisWindow)
    {
        for (auto& weight : normalizationWindow)
            weight *= weight;
    }
    const auto cola = validateConstantOverlapAdd(
        normalizationWindow, configuration_.hopSize);
    if (!cola.valid)
        throw std::invalid_argument("Native STFT configuration failed COLA validation");
    steadyStateNormalizationWeight_ = cola.constantSum;
}

NativeStftStatus PreparedInverseStft::process(
    std::span<const ComplexSpectrumFrame> input,
    SpectralFrameTimestamp blockStart,
    std::size_t compiledInputLatencyFrames,
    std::span<double> output) noexcept
{
    if (input.size() > maximumFramesPerProcess_)
        return NativeStftStatus::InputCapacityExceeded;
    if (initialized_)
    {
        if (blockStart.transportEpoch != segmentStart_.transportEpoch
            || blockStart.discontinuitySequence
                != segmentStart_.discontinuitySequence
            || blockStart.absoluteFrame != nextBlockAbsoluteFrame_
            || compiledInputLatencyFrames
                != preparedInputLatencyFrames_)
        {
            return NativeStftStatus::DiscontinuousBlock;
        }
    }

    std::uint64_t blockEnd = 0;
    if (!addWithoutOverflow(
            blockStart.absoluteFrame,
            static_cast<std::uint64_t>(output.size()),
            blockEnd))
    {
        return NativeStftStatus::TimestampOverflow;
    }
    auto expectedFrameStart = initialized_
        ? nextExpectedFrameStart_
        : blockStart.absoluteFrame;
    auto expectedSourceSequence = initialized_
        ? nextExpectedSourceSequence_
        : std::uint64_t{0};
    std::size_t frameIndex = 0;
    while (true)
    {
        std::uint64_t arrival = 0;
        if (!addWithoutOverflow(
                expectedFrameStart,
                static_cast<std::uint64_t>(compiledInputLatencyFrames),
                arrival))
        {
            return NativeStftStatus::TimestampOverflow;
        }
        if (arrival >= blockEnd)
            break;
        if (arrival < blockStart.absoluteFrame)
            return NativeStftStatus::MissingFrame;
        if (frameIndex >= input.size())
            return NativeStftStatus::MissingFrame;

        const auto& frame = input[frameIndex];
        if (frame.formatVersion != complexSpectrumFormatVersion
            || frame.analysisTimestamp.transportEpoch
                != blockStart.transportEpoch
            || frame.analysisTimestamp.discontinuitySequence
                != blockStart.discontinuitySequence
            || frame.analysisTimestamp.absoluteFrame
                != expectedFrameStart
            || frame.sourceSequence != expectedSourceSequence
            || frame.frameSize != configuration_.frameSize
            || frame.hopSize != configuration_.hopSize
            || frame.window != configuration_.window
            || frame.channelIndex != 0
            || frame.bins.size()
                != configuration_.frameSize / 2 + 1
            || !std::ranges::all_of(
                frame.bins,
                [](const auto& value) {
                    return std::isfinite(value.real())
                        && std::isfinite(value.imag());
                }))
        {
            return NativeStftStatus::InvalidFrame;
        }
        if (materiallyNonReal(frame.bins.front())
            || materiallyNonReal(frame.bins.back()))
        {
            return NativeStftStatus::NonRealEndpoint;
        }

        auto transformed = std::span<std::complex<double>>{
            transformedFrames_}.subspan(
                frameIndex * configuration_.frameSize,
                configuration_.frameSize);
        transformed.front() = {frame.bins.front().real(), 0.0};
        transformed[configuration_.frameSize / 2] = {
            frame.bins.back().real(),
            0.0,
        };
        for (std::size_t bin = 1;
             bin < configuration_.frameSize / 2;
             ++bin)
        {
            transformed[bin] = frame.bins[bin];
            transformed[configuration_.frameSize - bin] =
                std::conj(frame.bins[bin]);
        }
        if (!plan_.inverse(transformed))
            return NativeStftStatus::InvalidFrame;
        const auto safeOverlapMagnitude =
            std::numeric_limits<double>::max() / 8.0;
        if (!std::ranges::all_of(
                transformed,
                [&](const auto& value) {
                    return std::isfinite(value.real())
                        && std::isfinite(value.imag())
                        && std::abs(value.real())
                            <= safeOverlapMagnitude;
                }))
        {
            return NativeStftStatus::NumericOverflow;
        }

        ++frameIndex;
        if (expectedSourceSequence
            == std::numeric_limits<std::uint64_t>::max())
        {
            return NativeStftStatus::TimestampOverflow;
        }
        ++expectedSourceSequence;
        if (!addWithoutOverflow(
                expectedFrameStart,
                static_cast<std::uint64_t>(configuration_.hopSize),
                expectedFrameStart))
        {
            return NativeStftStatus::TimestampOverflow;
        }
    }
    if (frameIndex != input.size())
        return NativeStftStatus::UnexpectedFrame;

    std::ranges::copy(
        overlapSamples_,
        preflightOverlapSamples_.begin());
    std::ranges::copy(
        overlapWeights_,
        preflightOverlapWeights_.begin());
    auto preflightReadIndex = overlapReadIndex_;
    frameIndex = 0;
    for (std::size_t outputIndex = 0;
         outputIndex < output.size();
         ++outputIndex)
    {
        const auto absoluteFrame = blockStart.absoluteFrame
            + static_cast<std::uint64_t>(outputIndex);
        if (frameIndex < input.size())
        {
            std::uint64_t arrival = 0;
            if (!addWithoutOverflow(
                    input[frameIndex].analysisTimestamp.absoluteFrame,
                    static_cast<std::uint64_t>(compiledInputLatencyFrames),
                    arrival))
            {
                return NativeStftStatus::TimestampOverflow;
            }
            if (arrival == absoluteFrame)
            {
                const auto transformed =
                    std::span<const std::complex<double>>{
                        transformedFrames_}.subspan(
                            frameIndex * configuration_.frameSize,
                            configuration_.frameSize);
                for (std::size_t frame = 0;
                     frame < configuration_.frameSize;
                     ++frame)
                {
                    const auto overlapIndex =
                        (preflightReadIndex + frame)
                        % configuration_.frameSize;
                    const auto synthesisWeight = synthesisWindowPolicy_
                            == InverseStftSynthesisWindowPolicy::
                                ApplyAnalysisWindow
                        ? window_[frame]
                        : 1.0;
                    const auto normalizationWeight =
                        window_[frame] * synthesisWeight;
                    const auto sample =
                        preflightOverlapSamples_[overlapIndex]
                        + transformed[frame].real()
                            * synthesisWeight;
                    const auto weight =
                        preflightOverlapWeights_[overlapIndex]
                        + normalizationWeight;
                    if (!std::isfinite(sample)
                        || !std::isfinite(weight))
                    {
                        return NativeStftStatus::NumericOverflow;
                    }
                    preflightOverlapSamples_[overlapIndex] = sample;
                    preflightOverlapWeights_[overlapIndex] = weight;
                }
                ++frameIndex;
            }
        }

        const auto weight = synthesisWindowPolicy_
                == InverseStftSynthesisWindowPolicy::ApplyAnalysisWindow
            ? steadyStateNormalizationWeight_
            : preflightOverlapWeights_[preflightReadIndex];
        if (weight > 0.0
            && !std::isfinite(
                preflightOverlapSamples_[preflightReadIndex]
                / weight))
        {
            return NativeStftStatus::NumericOverflow;
        }
        preflightOverlapSamples_[preflightReadIndex] = 0.0;
        preflightOverlapWeights_[preflightReadIndex] = 0.0;
        preflightReadIndex = (preflightReadIndex + 1)
            % configuration_.frameSize;
    }

    frameIndex = 0;
    for (std::size_t outputIndex = 0;
         outputIndex < output.size();
         ++outputIndex)
    {
        const auto absoluteFrame = blockStart.absoluteFrame
            + static_cast<std::uint64_t>(outputIndex);
        if (frameIndex < input.size())
        {
            std::uint64_t arrival = 0;
            if (!addWithoutOverflow(
                    input[frameIndex].analysisTimestamp.absoluteFrame,
                    static_cast<std::uint64_t>(compiledInputLatencyFrames),
                    arrival))
            {
                return NativeStftStatus::TimestampOverflow;
            }
            if (arrival == absoluteFrame)
            {
                const auto transformed =
                    std::span<const std::complex<double>>{
                        transformedFrames_}.subspan(
                            frameIndex * configuration_.frameSize,
                            configuration_.frameSize);
                for (std::size_t frame = 0;
                     frame < configuration_.frameSize;
                     ++frame)
                {
                    const auto overlapIndex =
                        (overlapReadIndex_ + frame)
                        % configuration_.frameSize;
                    const auto synthesisWeight = synthesisWindowPolicy_
                            == InverseStftSynthesisWindowPolicy::
                                ApplyAnalysisWindow
                        ? window_[frame]
                        : 1.0;
                    overlapSamples_[overlapIndex] +=
                        transformed[frame].real()
                            * synthesisWeight;
                    overlapWeights_[overlapIndex] +=
                        window_[frame] * synthesisWeight;
                }
                ++frameIndex;
            }
        }

        const auto weight = synthesisWindowPolicy_
                == InverseStftSynthesisWindowPolicy::ApplyAnalysisWindow
            ? steadyStateNormalizationWeight_
            : overlapWeights_[overlapReadIndex_];
        output[outputIndex] = weight > 0.0
            ? overlapSamples_[overlapReadIndex_] / weight
            : 0.0;
        overlapSamples_[overlapReadIndex_] = 0.0;
        overlapWeights_[overlapReadIndex_] = 0.0;
        overlapReadIndex_ = (overlapReadIndex_ + 1)
            % configuration_.frameSize;
    }

    if (!initialized_)
    {
        segmentStart_ = blockStart;
        preparedInputLatencyFrames_ = compiledInputLatencyFrames;
        initialized_ = true;
    }
    nextExpectedFrameStart_ = expectedFrameStart;
    nextExpectedSourceSequence_ = expectedSourceSequence;
    nextBlockAbsoluteFrame_ = blockEnd;
    return NativeStftStatus::Ok;
}

void PreparedInverseStft::reset() noexcept
{
    std::ranges::fill(
        transformedFrames_, std::complex<double>{});
    std::ranges::fill(overlapSamples_, 0.0);
    std::ranges::fill(overlapWeights_, 0.0);
    std::ranges::fill(preflightOverlapSamples_, 0.0);
    std::ranges::fill(preflightOverlapWeights_, 0.0);
    overlapReadIndex_ = 0;
    nextExpectedFrameStart_ = 0;
    nextExpectedSourceSequence_ = 0;
    nextBlockAbsoluteFrame_ = 0;
    preparedInputLatencyFrames_ = 0;
    segmentStart_ = {};
    initialized_ = false;
}

bool PreparedInverseStft::copyProcessingStateFrom(
    const PreparedInverseStft& source) noexcept
{
    if (configuration_ != source.configuration_
        || maximumFramesPerProcess_ != source.maximumFramesPerProcess_
        || synthesisWindowPolicy_ != source.synthesisWindowPolicy_
        || transformedFrames_.size() != source.transformedFrames_.size()
        || overlapSamples_.size() != source.overlapSamples_.size()
        || overlapWeights_.size() != source.overlapWeights_.size()
        || preflightOverlapSamples_.size()
            != source.preflightOverlapSamples_.size()
        || preflightOverlapWeights_.size()
            != source.preflightOverlapWeights_.size())
    {
        return false;
    }
    std::ranges::copy(source.transformedFrames_, transformedFrames_.begin());
    std::ranges::copy(source.overlapSamples_, overlapSamples_.begin());
    std::ranges::copy(source.overlapWeights_, overlapWeights_.begin());
    std::ranges::copy(
        source.preflightOverlapSamples_, preflightOverlapSamples_.begin());
    std::ranges::copy(
        source.preflightOverlapWeights_, preflightOverlapWeights_.begin());
    overlapReadIndex_ = source.overlapReadIndex_;
    nextExpectedFrameStart_ = source.nextExpectedFrameStart_;
    nextExpectedSourceSequence_ = source.nextExpectedSourceSequence_;
    nextBlockAbsoluteFrame_ = source.nextBlockAbsoluteFrame_;
    preparedInputLatencyFrames_ = source.preparedInputLatencyFrames_;
    segmentStart_ = source.segmentStart_;
    initialized_ = source.initialized_;
    return true;
}

bool PreparedInverseStft::retimeNextBlockAbsoluteFrame(
    std::uint64_t absoluteFrame) noexcept
{
    if (!initialized_)
        return true;
    if (absoluteFrame < nextBlockAbsoluteFrame_)
        return false;
    const auto delta = absoluteFrame - nextBlockAbsoluteFrame_;
    if (segmentStart_.absoluteFrame
            > std::numeric_limits<std::uint64_t>::max() - delta
        || nextExpectedFrameStart_
            > std::numeric_limits<std::uint64_t>::max() - delta)
    {
        return false;
    }
    segmentStart_.absoluteFrame += delta;
    nextExpectedFrameStart_ += delta;
    nextBlockAbsoluteFrame_ = absoluteFrame;
    return true;
}

const NativeStftConfiguration&
PreparedInverseStft::configuration() const noexcept
{
    return configuration_;
}

std::size_t PreparedInverseStft::maximumFramesPerProcess() const noexcept
{
    return maximumFramesPerProcess_;
}
}
